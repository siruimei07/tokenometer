use std::{
    ffi::OsStr,
    fs::{self, File, Metadata, OpenOptions},
    io,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::domain::{ReportingTimeZoneSource, ReportingTimeZoneView};

const FILE_ATTRIBUTE_REPARSE_POINT_VALUE: u32 = 0x0000_0400;

#[derive(Debug, Error)]
pub enum PlatformError {
    #[error("the configured source root must be absolute")]
    RootNotAbsolute,
    #[error("the configured source root is unavailable")]
    RootUnavailable,
    #[error("the path is outside the configured source root")]
    PathOutsideRoot,
    #[error("the path is not in the Codex read allowlist")]
    PathNotAllowed,
    #[error("reparse points are not accepted at this trust boundary")]
    ReparsePoint,
    #[error("the source is not a regular file")]
    NotRegularFile,
    #[error("the local data directory could not be secured")]
    DirectorySecurity,
    #[error("a local file operation failed")]
    Io(#[source] io::Error),
}

impl From<io::Error> for PlatformError {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileIdentity {
    pub value: String,
    pub degraded: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum CodexFileKind {
    Active,
    Archive,
    Index,
}

impl CodexFileKind {
    pub fn as_db_str(self) -> &'static str {
        match self {
            Self::Active => "active",
            Self::Archive => "archive",
            Self::Index => "index",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveredCodexFile {
    pub path: PathBuf,
    pub logical_file_id: String,
    pub kind: CodexFileKind,
}

pub struct OpenedCodexFile {
    pub file: File,
    pub canonical_path: PathBuf,
    pub identity: FileIdentity,
}

pub fn now_ms() -> i64 {
    chrono::Utc::now().timestamp_millis().max(0)
}

pub fn reporting_time_zone() -> ReportingTimeZoneView {
    #[cfg(windows)]
    {
        use std::mem::MaybeUninit;
        use windows_sys::Win32::System::Time::{
            DYNAMIC_TIME_ZONE_INFORMATION, GetDynamicTimeZoneInformation,
        };

        let mut information = MaybeUninit::<DYNAMIC_TIME_ZONE_INFORMATION>::zeroed();
        // SAFETY: Windows initializes the complete output structure before returning.
        unsafe {
            GetDynamicTimeZoneInformation(information.as_mut_ptr());
            let information = information.assume_init();
            let id = utf16_z(&information.TimeZoneKeyName)
                .filter(|value| !value.is_empty())
                .unwrap_or_else(|| "Windows system time zone".to_string());
            let display_name = utf16_z(&information.StandardName)
                .filter(|value| !value.is_empty())
                .unwrap_or_else(|| id.clone());
            ReportingTimeZoneView {
                id,
                display_name,
                source: ReportingTimeZoneSource::WindowsSystem,
            }
        }
    }

    #[cfg(not(windows))]
    {
        let offset = chrono::Local::now().format("UTC%:z").to_string();
        ReportingTimeZoneView {
            id: offset.clone(),
            display_name: offset,
            source: ReportingTimeZoneSource::WindowsSystem,
        }
    }
}

#[cfg(windows)]
fn utf16_z(value: &[u16]) -> Option<String> {
    let length = value
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(value.len());
    String::from_utf16(&value[..length]).ok()
}

pub fn resolve_codex_root() -> Result<PathBuf, PlatformError> {
    let configured = configured_codex_root()?;
    normalize_source_root(&configured)
}

pub fn configured_codex_root() -> Result<PathBuf, PlatformError> {
    let configured = std::env::var_os("CODEX_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("USERPROFILE").map(|profile| PathBuf::from(profile).join(".codex"))
        })
        .ok_or(PlatformError::RootUnavailable)?;
    if !configured.is_absolute() {
        return Err(PlatformError::RootNotAbsolute);
    }
    Ok(configured)
}

pub fn normalize_source_root(path: &Path) -> Result<PathBuf, PlatformError> {
    if !path.is_absolute() {
        return Err(PlatformError::RootNotAbsolute);
    }
    if !path.exists() {
        return Err(PlatformError::RootUnavailable);
    }
    reject_reparse(path)?;
    let canonical = fs::canonicalize(path).map_err(|_| PlatformError::RootUnavailable)?;
    if !canonical.is_dir() {
        return Err(PlatformError::RootUnavailable);
    }
    Ok(canonical)
}

pub fn prepare_private_app_dir(path: &Path) -> Result<PathBuf, PlatformError> {
    if !path.is_absolute() {
        return Err(PlatformError::RootNotAbsolute);
    }
    fs::create_dir_all(path)?;
    reject_reparse(path)?;
    let canonical = fs::canonicalize(path)?;
    reject_reparse(&canonical)?;
    harden_private_directory(&canonical)?;
    Ok(canonical)
}

pub fn validate_private_data_file(path: &Path) -> Result<(), PlatformError> {
    if !path.exists() {
        return Ok(());
    }
    reject_reparse(path)?;
    if !fs::symlink_metadata(path)?.is_file() {
        return Err(PlatformError::NotRegularFile);
    }
    Ok(())
}

pub fn discover_codex_files(
    canonical_root: &Path,
) -> Result<Vec<DiscoveredCodexFile>, PlatformError> {
    reject_reparse(canonical_root)?;
    let mut discovered = Vec::new();
    let sessions = canonical_root.join("sessions");
    if sessions.is_dir()
        && fs::symlink_metadata(&sessions).is_ok_and(|metadata| !is_reparse(&metadata))
    {
        let _ = discover_session_tree(canonical_root, &sessions, 0, &mut discovered);
    }

    let archive = canonical_root.join("archived_sessions");
    if archive.is_dir()
        && fs::symlink_metadata(&archive).is_ok_and(|metadata| !is_reparse(&metadata))
        && let Ok(entries) = fs::read_dir(&archive)
    {
        for entry in entries {
            let Ok(entry) = entry else {
                continue;
            };
            let path = entry.path();
            let Ok(metadata) = fs::symlink_metadata(&path) else {
                continue;
            };
            if is_reparse(&metadata) || !metadata.is_file() || !is_rollout_file(&path) {
                continue;
            }
            discovered.push(discovered_rollout(
                canonical_root,
                path,
                CodexFileKind::Archive,
            ));
        }
    }

    let index = canonical_root.join("session_index.jsonl");
    if index.is_file()
        && fs::symlink_metadata(&index)
            .is_ok_and(|metadata| !is_reparse(&metadata) && metadata.is_file())
    {
        discovered.push(DiscoveredCodexFile {
            path: index,
            logical_file_id: "index/v1".to_string(),
            kind: CodexFileKind::Index,
        });
    }

    discovered.sort_by(|left, right| {
        file_kind_priority(left.kind)
            .cmp(&file_kind_priority(right.kind))
            .then_with(|| left.logical_file_id.cmp(&right.logical_file_id))
            .then_with(|| left.path.cmp(&right.path))
    });
    Ok(discovered)
}

fn discover_session_tree(
    root: &Path,
    directory: &Path,
    depth: usize,
    discovered: &mut Vec<DiscoveredCodexFile>,
) -> Result<(), PlatformError> {
    if depth > 12 {
        return Ok(());
    }
    for entry in fs::read_dir(directory)? {
        let Ok(entry) = entry else {
            continue;
        };
        let path = entry.path();
        let Ok(metadata) = fs::symlink_metadata(&path) else {
            continue;
        };
        if is_reparse(&metadata) {
            continue;
        }
        if metadata.is_dir() {
            let _ = discover_session_tree(root, &path, depth + 1, discovered);
        } else if metadata.is_file()
            && is_rollout_file(&path)
            && let Ok(canonical) = fs::canonicalize(&path)
            && canonical.starts_with(root)
        {
            discovered.push(discovered_rollout(root, canonical, CodexFileKind::Active));
        }
    }
    Ok(())
}

fn discovered_rollout(root: &Path, path: PathBuf, kind: CodexFileKind) -> DiscoveredCodexFile {
    DiscoveredCodexFile {
        logical_file_id: pending_path_key(root, &path),
        path,
        kind,
    }
}

fn pending_path_key(root: &Path, path: &Path) -> String {
    let relative = path.strip_prefix(root).unwrap_or(path);
    let mut hasher = Sha256::new();
    hasher.update(b"tokenometer/codex-pending-path/v1");

    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        for unit in relative.as_os_str().encode_wide() {
            hasher.update(unit.to_le_bytes());
        }
    }
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        hasher.update(relative.as_os_str().as_bytes());
    }
    #[cfg(not(any(windows, unix)))]
    hasher.update(relative.to_string_lossy().as_bytes());

    let digest = hasher.finalize();
    let mut encoded = String::with_capacity(digest.len() * 2);
    for byte in digest {
        use std::fmt::Write;
        let _ = write!(encoded, "{byte:02x}");
    }
    format!("pending/{encoded}")
}

fn file_kind_priority(kind: CodexFileKind) -> u8 {
    match kind {
        CodexFileKind::Active => 0,
        CodexFileKind::Archive => 1,
        CodexFileKind::Index => 2,
    }
}

pub fn validate_allowed_file(
    canonical_root: &Path,
    candidate: &Path,
) -> Result<PathBuf, PlatformError> {
    if !candidate.is_absolute() {
        return Err(PlatformError::PathOutsideRoot);
    }
    reject_reparse(candidate)?;
    let canonical = fs::canonicalize(candidate)?;
    if !canonical.starts_with(canonical_root) {
        return Err(PlatformError::PathOutsideRoot);
    }
    let metadata = fs::symlink_metadata(&canonical)?;
    if is_reparse(&metadata) {
        return Err(PlatformError::ReparsePoint);
    }
    if !metadata.is_file() {
        return Err(PlatformError::NotRegularFile);
    }
    if classify_allowed_file(canonical_root, &canonical).is_none() {
        return Err(PlatformError::PathNotAllowed);
    }
    Ok(canonical)
}

pub fn open_allowed_file(
    canonical_root: &Path,
    candidate: &Path,
) -> Result<OpenedCodexFile, PlatformError> {
    let validated = validate_allowed_file(canonical_root, candidate)?;
    let file = OpenOptions::new().read(true).open(&validated)?;
    let canonical_path = final_path_for_open_file(&file, &validated)?;
    if !canonical_path.starts_with(canonical_root) {
        return Err(PlatformError::PathOutsideRoot);
    }
    reject_reparse(&canonical_path)?;
    if classify_allowed_file(canonical_root, &canonical_path).is_none() {
        return Err(PlatformError::PathNotAllowed);
    }
    let identity = file_identity(&file, &canonical_path)?;
    Ok(OpenedCodexFile {
        file,
        canonical_path,
        identity,
    })
}

#[cfg(windows)]
fn final_path_for_open_file(file: &File, fallback: &Path) -> Result<PathBuf, PlatformError> {
    use std::{
        ffi::OsString,
        os::windows::{ffi::OsStringExt, io::AsRawHandle},
    };
    use windows_sys::Win32::Storage::FileSystem::{
        FILE_NAME_NORMALIZED, GetFinalPathNameByHandleW, VOLUME_NAME_DOS,
    };

    let handle = file.as_raw_handle();
    let mut buffer = vec![0u16; 32_768];
    // SAFETY: the handle belongs to `file`, and the buffer is writable for the
    // length passed to Windows. The returned path is copied before either drops.
    let length = unsafe {
        GetFinalPathNameByHandleW(
            handle,
            buffer.as_mut_ptr(),
            u32::try_from(buffer.len()).map_err(|_| PlatformError::RootUnavailable)?,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS,
        )
    };
    if length == 0 {
        return Err(PlatformError::Io(io::Error::last_os_error()));
    }
    let length = usize::try_from(length).map_err(|_| PlatformError::RootUnavailable)?;
    if length >= buffer.len() {
        return fs::canonicalize(fallback).map_err(PlatformError::from);
    }
    buffer.truncate(length);
    Ok(PathBuf::from(OsString::from_wide(&buffer)))
}

#[cfg(not(windows))]
fn final_path_for_open_file(_file: &File, fallback: &Path) -> Result<PathBuf, PlatformError> {
    fs::canonicalize(fallback).map_err(PlatformError::from)
}

pub fn classify_allowed_file(root: &Path, path: &Path) -> Option<CodexFileKind> {
    let relative = path.strip_prefix(root).ok()?;
    let components: Vec<_> = relative.components().collect();
    if components.len() == 1 && relative == Path::new("session_index.jsonl") {
        return Some(CodexFileKind::Index);
    }
    if components.len() == 2
        && components[0].as_os_str() == "archived_sessions"
        && is_rollout_file(path)
    {
        return Some(CodexFileKind::Archive);
    }
    if components.len() >= 2 && components[0].as_os_str() == "sessions" && is_rollout_file(path) {
        return Some(CodexFileKind::Active);
    }
    None
}

pub fn file_identity(file: &File, _canonical_path: &Path) -> Result<FileIdentity, PlatformError> {
    let metadata = file.metadata()?;
    if !metadata.is_file() {
        return Err(PlatformError::NotRegularFile);
    }

    #[cfg(windows)]
    {
        use std::{mem::MaybeUninit, os::windows::io::AsRawHandle};
        use windows_sys::Win32::Storage::FileSystem::{
            BY_HANDLE_FILE_INFORMATION, GetFileInformationByHandle,
        };

        let mut information = MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::zeroed();
        // SAFETY: `file` owns a valid handle and Windows initializes the output
        // structure when the call succeeds.
        let succeeded =
            unsafe { GetFileInformationByHandle(file.as_raw_handle(), information.as_mut_ptr()) };
        if succeeded == 0 {
            return Err(PlatformError::Io(io::Error::last_os_error()));
        }
        // SAFETY: success means Windows initialized the complete structure.
        let information = unsafe { information.assume_init() };
        let index =
            (u64::from(information.nFileIndexHigh) << 32) | u64::from(information.nFileIndexLow);
        return Ok(FileIdentity {
            value: format!(
                "windows:{:08x}:{index:016x}",
                information.dwVolumeSerialNumber
            ),
            degraded: false,
        });
    }

    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        return Ok(FileIdentity {
            value: format!("unix:{:016x}:{:016x}", metadata.dev(), metadata.ino()),
            degraded: false,
        });
    }

    #[allow(unreachable_code)]
    Ok(FileIdentity {
        value: format!("degraded:{}", _canonical_path.to_string_lossy()),
        degraded: true,
    })
}

pub fn metadata_modified_ms(metadata: &Metadata) -> Option<i64> {
    system_time_ms(metadata.modified().ok()?)
}

pub fn system_time_ms(value: SystemTime) -> Option<i64> {
    let millis = value.duration_since(UNIX_EPOCH).ok()?.as_millis();
    i64::try_from(millis).ok()
}

pub fn safe_project_label(path: &str) -> Option<String> {
    let value = Path::new(path)
        .file_name()
        .and_then(OsStr::to_str)?
        .chars()
        .filter(|character| !character.is_control() && !is_bidi_control(*character))
        .take(80)
        .collect::<String>();
    (!value.trim().is_empty()).then_some(value)
}

fn is_bidi_control(character: char) -> bool {
    matches!(
        character,
        '\u{061c}'
            | '\u{200e}'
            | '\u{200f}'
            | '\u{202a}'..='\u{202e}'
            | '\u{2066}'..='\u{2069}'
    )
}

fn is_rollout_file(path: &Path) -> bool {
    path.file_name()
        .and_then(OsStr::to_str)
        .is_some_and(|name| name.starts_with("rollout-") && name.ends_with(".jsonl"))
}

fn reject_reparse(path: &Path) -> Result<(), PlatformError> {
    let metadata = fs::symlink_metadata(path)?;
    if is_reparse(&metadata) {
        return Err(PlatformError::ReparsePoint);
    }
    Ok(())
}

fn is_reparse(metadata: &Metadata) -> bool {
    if metadata.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT_VALUE != 0
    }
    #[cfg(not(windows))]
    {
        let _ = FILE_ATTRIBUTE_REPARSE_POINT_VALUE;
        false
    }
}

#[cfg(windows)]
fn harden_private_directory(path: &Path) -> Result<(), PlatformError> {
    use std::{os::windows::ffi::OsStrExt, ptr};
    use windows_sys::Win32::{
        Foundation::LocalFree,
        Security::{
            Authorization::{
                ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
            },
            DACL_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR,
            SetFileSecurityW,
        },
    };

    let descriptor_text: Vec<u16> =
        OsStr::new("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)")
            .encode_wide()
            .chain(Some(0))
            .collect();
    let path_wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut descriptor: PSECURITY_DESCRIPTOR = ptr::null_mut();

    // SAFETY: Both input strings are NUL terminated, Windows allocates the returned descriptor,
    // and it is released with LocalFree on every path after successful conversion.
    let converted = unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor_text.as_ptr(),
            SDDL_REVISION_1,
            &mut descriptor,
            ptr::null_mut(),
        )
    };
    if converted == 0 || descriptor.is_null() {
        return Err(PlatformError::DirectorySecurity);
    }

    // SAFETY: `descriptor` is a valid self-relative descriptor returned by Windows and
    // `path_wide` remains alive for the duration of the call.
    let secured = unsafe {
        SetFileSecurityW(
            path_wide.as_ptr(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor,
        )
    };
    // SAFETY: Windows allocated `descriptor` for the caller with LocalAlloc.
    unsafe {
        LocalFree(descriptor.cast());
    }
    if secured == 0 {
        return Err(PlatformError::DirectorySecurity);
    }
    Ok(())
}

#[cfg(not(windows))]
fn harden_private_directory(_path: &Path) -> Result<(), PlatformError> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::fs;

    use tempfile::tempdir;

    use super::{
        CodexFileKind, classify_allowed_file, discover_codex_files, safe_project_label,
        validate_allowed_file,
    };

    #[test]
    fn discovery_only_returns_allowlisted_codex_files() {
        let root = tempdir().unwrap();
        let session_dir = root.path().join("sessions/2026/08/08");
        let archive_dir = root.path().join("archived_sessions");
        fs::create_dir_all(&session_dir).unwrap();
        fs::create_dir_all(&archive_dir).unwrap();
        fs::write(session_dir.join("rollout-one.jsonl"), b"{}").unwrap();
        fs::write(archive_dir.join("rollout-two.jsonl"), b"{}").unwrap();
        fs::write(root.path().join("session_index.jsonl"), b"{}").unwrap();
        fs::write(root.path().join("auth.json"), b"secret").unwrap();

        let canonical = fs::canonicalize(root.path()).unwrap();
        let files = discover_codex_files(&canonical).unwrap();
        assert_eq!(files.len(), 3);
        assert!(files.iter().all(|file| !file.path.ends_with("auth.json")));
    }

    #[test]
    fn discovery_does_not_collapse_equal_filenames_in_different_session_directories() {
        let root = tempdir().unwrap();
        let first = root.path().join("sessions/2026/08/08/a");
        let second = root.path().join("sessions/2026/08/08/b");
        fs::create_dir_all(&first).unwrap();
        fs::create_dir_all(&second).unwrap();
        fs::write(first.join("rollout-same.jsonl"), b"{}").unwrap();
        fs::write(second.join("rollout-same.jsonl"), b"{}").unwrap();

        let canonical = fs::canonicalize(root.path()).unwrap();
        let files = discover_codex_files(&canonical).unwrap();
        assert_eq!(files.len(), 2);
        assert_ne!(files[0].logical_file_id, files[1].logical_file_id);
    }

    #[test]
    fn arbitrary_files_are_rejected_even_when_inside_root() {
        let root = tempdir().unwrap();
        let candidate = root.path().join("auth.json");
        fs::write(&candidate, b"secret").unwrap();
        let canonical = fs::canonicalize(root.path()).unwrap();

        assert!(validate_allowed_file(&canonical, &candidate).is_err());
        assert_eq!(classify_allowed_file(&canonical, &candidate), None);
    }

    #[test]
    fn archive_allowlist_is_not_recursive() {
        let root = tempdir().unwrap();
        let nested = root.path().join("archived_sessions/nested");
        fs::create_dir_all(&nested).unwrap();
        let candidate = nested.join("rollout-hidden.jsonl");
        fs::write(&candidate, b"{}").unwrap();
        let canonical = fs::canonicalize(root.path()).unwrap();
        let candidate = fs::canonicalize(candidate).unwrap();

        assert_eq!(classify_allowed_file(&canonical, &candidate), None);
        assert!(validate_allowed_file(&canonical, &candidate).is_err());
    }

    #[test]
    fn active_rollouts_may_be_nested() {
        let root = tempdir().unwrap();
        let nested = root.path().join("sessions/2026/08/08");
        fs::create_dir_all(&nested).unwrap();
        let candidate = nested.join("rollout-test.jsonl");
        fs::write(&candidate, b"{}").unwrap();
        let canonical = fs::canonicalize(root.path()).unwrap();
        let candidate = fs::canonicalize(candidate).unwrap();

        assert_eq!(
            classify_allowed_file(&canonical, &candidate),
            Some(CodexFileKind::Active)
        );
    }

    #[test]
    fn project_labels_strip_control_and_bidi_characters() {
        assert_eq!(
            safe_project_label("C:/synthetic/proj\u{202e}ect\n"),
            Some("project".to_string())
        );
    }
}
