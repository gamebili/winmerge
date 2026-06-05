use calamine::{open_workbook_auto, Data, Range, Reader};
use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::ffi::OsStr;
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::panic;
use std::path::{Path, PathBuf};
use std::slice;
use std::time::Instant;

pub type Result<T> = std::result::Result<T, Box<dyn Error>>;

const SUPPORTED_EXTENSIONS: &[&str] = &[
    "xls", "xlsx", "xlsm", "xlsb", "xla", "xlam", "xlt", "xltx", "xltm",
];
const MAX_WIDE_STRING_UNITS: usize = 32768;

pub fn is_supported_workbook(path: &Path) -> bool {
    path.extension()
        .and_then(OsStr::to_str)
        .map(|extension| {
            let lower = extension.to_ascii_lowercase();
            SUPPORTED_EXTENSIONS.contains(&lower.as_str())
        })
        .unwrap_or(false)
}

pub fn unpack_folder(source: &Path, destination: &Path) -> Result<()> {
    unpack_folder_with_diagnostics(source, destination, excel_log_enabled()).map(|_| ())
}

pub fn unpack_folder_with_diagnostics(
    source: &Path,
    destination: &Path,
    log_enabled: bool,
) -> Result<String> {
    if !is_supported_workbook(source) {
        return Err(format!("unsupported Excel workbook: {}", source.display()).into());
    }

    let mut diagnostics = String::new();
    let total_start = Instant::now();
    log_line(
        log_enabled,
        &mut diagnostics,
        format!(
            "[excel2tsv] unpack-folder start source=\"{}\" destination=\"{}\"",
            source.display(),
            destination.display()
        ),
    );

    fs::create_dir_all(destination)?;

    let open_start = Instant::now();
    let mut workbook = open_workbook_auto(source)?;
    log_line(
        log_enabled,
        &mut diagnostics,
        format!(
            "[excel2tsv] open-workbook elapsed={}ms",
            open_start.elapsed().as_millis()
        ),
    );

    let sheet_names = workbook.sheet_names().to_owned();
    log_line(
        log_enabled,
        &mut diagnostics,
        format!("[excel2tsv] sheet-count={}", sheet_names.len()),
    );

    let mut used_names = HashMap::new();
    for sheet_name in sheet_names {
        let sheet_start = Instant::now();
        let range = workbook.worksheet_range(&sheet_name)?;
        let read_elapsed = sheet_start.elapsed().as_millis();
        let write_start = Instant::now();
        let file_name = unique_sheet_file_name(&sheet_name, &mut used_names);
        let destination_file = destination.join(file_name);
        write_range_as_tsv(&range, &destination_file)?;
        let (height, width) = range.get_size();
        log_line(
            log_enabled,
            &mut diagnostics,
            format!(
                "[excel2tsv] sheet name=\"{}\" rows={} columns={} read={}ms write={}ms",
                sheet_name,
                height,
                width,
                read_elapsed,
                write_start.elapsed().as_millis()
            ),
        );
    }

    log_line(
        log_enabled,
        &mut diagnostics,
        format!(
            "[excel2tsv] unpack-folder done elapsed={}ms",
            total_start.elapsed().as_millis()
        ),
    );

    Ok(diagnostics)
}

pub fn excel_log_enabled() -> bool {
    env::var_os("WINMERGE_EXCEL_LOG")
        .and_then(|value| value.into_string().ok())
        .map(|value| {
            let value = value.trim().to_ascii_lowercase();
            !(value.is_empty()
                || value == "0"
                || value == "false"
                || value == "off"
                || value == "no")
        })
        .unwrap_or(false)
}

fn log_line(enabled: bool, diagnostics: &mut String, line: String) {
    if enabled {
        diagnostics.push_str(&line);
        diagnostics.push('\n');
    }
}

fn unique_sheet_file_name(sheet_name: &str, used_names: &mut HashMap<String, usize>) -> String {
    let base_name = sanitize_windows_file_stem(sheet_name);
    let count = used_names.entry(base_name.clone()).or_insert(0);
    *count += 1;

    if *count == 1 {
        format!("{base_name}.tsv")
    } else {
        format!("{base_name} ({count}).tsv")
    }
}

fn sanitize_windows_file_stem(sheet_name: &str) -> String {
    let mut escaped = String::with_capacity(sheet_name.len());

    for ch in sheet_name.chars() {
        match ch {
            '%' => escaped.push_str("%25"),
            '<' => escaped.push_str("%3C"),
            '>' => escaped.push_str("%3E"),
            ':' => escaped.push_str("%3A"),
            '"' => escaped.push_str("%22"),
            '/' => escaped.push_str("%2F"),
            '\\' => escaped.push_str("%5C"),
            '|' => escaped.push_str("%7C"),
            '?' => escaped.push_str("%3F"),
            '*' => escaped.push_str("%2A"),
            ch if ch.is_control() => {
                for byte in ch.to_string().as_bytes() {
                    escaped.push_str(&format!("%{byte:02X}"));
                }
            }
            ch => escaped.push(ch),
        }
    }

    while escaped.ends_with(' ') || escaped.ends_with('.') {
        let ch = escaped.pop().unwrap_or_default();
        if ch == ' ' {
            escaped.push_str("%20");
        } else {
            escaped.push_str("%2E");
        }
    }

    if escaped.is_empty() {
        escaped.push_str("Sheet");
    }

    if is_reserved_windows_file_stem(&escaped) {
        escaped.insert(0, '%');
    }

    escaped
}

fn is_reserved_windows_file_stem(file_stem: &str) -> bool {
    let device_name = file_stem
        .trim_end_matches([' ', '.'])
        .split('.')
        .next()
        .unwrap_or_default()
        .to_ascii_uppercase();

    matches!(
        device_name.as_str(),
        "CON"
            | "PRN"
            | "AUX"
            | "NUL"
            | "COM1"
            | "COM2"
            | "COM3"
            | "COM4"
            | "COM5"
            | "COM6"
            | "COM7"
            | "COM8"
            | "COM9"
            | "LPT1"
            | "LPT2"
            | "LPT3"
            | "LPT4"
            | "LPT5"
            | "LPT6"
            | "LPT7"
            | "LPT8"
            | "LPT9"
    )
}

fn write_range_as_tsv(range: &Range<Data>, path: &Path) -> io::Result<()> {
    let mut row_widths = Vec::new();
    let mut last_non_empty_row = None;

    // Keep interior blank rows, but avoid producing noisy trailing tabs and rows.
    for (row_index, row) in range.rows().enumerate() {
        let width = row
            .iter()
            .rposition(|cell| !is_empty_cell(cell))
            .map(|index| index + 1)
            .unwrap_or_default();
        row_widths.push(width);
        if width > 0 {
            last_non_empty_row = Some(row_index);
        }
    }

    let file = File::create(path)?;
    let mut writer = BufWriter::new(file);
    writer.write_all(b"\xEF\xBB\xBF")?;

    let Some(last_row) = last_non_empty_row else {
        return Ok(());
    };

    for (row_index, row) in range.rows().enumerate() {
        if row_index > last_row {
            break;
        }

        let width = row_widths[row_index];
        for column_index in 0..width {
            if column_index > 0 {
                writer.write_all(b"\t")?;
            }

            let value = row
                .get(column_index)
                .map(cell_to_string)
                .unwrap_or_default();
            write_tsv_field(&mut writer, &value)?;
        }

        if row_index < last_row {
            writer.write_all(b"\n")?;
        }
    }

    Ok(())
}

fn is_empty_cell(cell: &Data) -> bool {
    matches!(cell, Data::Empty) || matches!(cell, Data::String(text) if text.is_empty())
}

fn cell_to_string(cell: &Data) -> String {
    match cell {
        Data::Empty => String::new(),
        Data::String(text) => text.clone(),
        Data::Float(value) => value.to_string(),
        Data::Int(value) => value.to_string(),
        Data::Bool(value) => {
            if *value {
                "TRUE".to_string()
            } else {
                "FALSE".to_string()
            }
        }
        _ => cell.to_string(),
    }
}

fn write_tsv_field(writer: &mut impl Write, value: &str) -> io::Result<()> {
    let needs_quotes = value.contains(['\t', '\r', '\n', '"']);
    if !needs_quotes {
        return writer.write_all(value.as_bytes());
    }

    writer.write_all(b"\"")?;
    for ch in value.chars() {
        if ch == '"' {
            writer.write_all(b"\"\"")?;
        } else {
            writer.write_all(ch.to_string().as_bytes())?;
        }
    }
    writer.write_all(b"\"")
}

#[no_mangle]
pub extern "system" fn excel2tsv_unpack_folder_utf16(
    source: *const u16,
    destination: *const u16,
    diagnostics: *mut u16,
    diagnostics_len: usize,
) -> i32 {
    let result = panic::catch_unwind(|| {
        let source = wide_ptr_to_path(source)?;
        let destination = wide_ptr_to_path(destination)?;
        unpack_folder_with_diagnostics(&source, &destination, excel_log_enabled())
            .map_err(|err| err.to_string())
    });

    match result {
        Ok(Ok(diagnostic_text)) => {
            write_wide_output(diagnostics, diagnostics_len, &diagnostic_text);
            0
        }
        Ok(Err(err)) => {
            write_wide_output(diagnostics, diagnostics_len, &err);
            1
        }
        Err(_) => {
            write_wide_output(diagnostics, diagnostics_len, "excel2tsv panicked");
            2
        }
    }
}

fn wide_ptr_to_path(ptr: *const u16) -> std::result::Result<PathBuf, String> {
    if ptr.is_null() {
        return Err("null path pointer".to_string());
    }

    let mut len = 0usize;
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
            if len > MAX_WIDE_STRING_UNITS {
                return Err("unterminated UTF-16 path".to_string());
            }
        }
        let units = slice::from_raw_parts(ptr, len);
        Ok(PathBuf::from(String::from_utf16_lossy(units)))
    }
}

fn write_wide_output(ptr: *mut u16, len: usize, text: &str) {
    if ptr.is_null() || len == 0 {
        return;
    }

    let mut units: Vec<u16> = text.encode_utf16().collect();
    if units.len() >= len {
        units.truncate(len - 1);
    }

    unsafe {
        if !units.is_empty() {
            std::ptr::copy_nonoverlapping(units.as_ptr(), ptr, units.len());
        }
        *ptr.add(units.len()) = 0;
    }
}
