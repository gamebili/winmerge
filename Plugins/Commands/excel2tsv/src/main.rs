use calamine::{open_workbook_auto, Data, Range, Reader};
use std::collections::HashMap;
use std::env;
use std::error::Error;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

const SUPPORTED_EXTENSIONS: &[&str] = &[
    "xls", "xlsx", "xlsm", "xlsb", "xla", "xlam", "xlt", "xltx", "xltm",
];

fn main() {
    if let Err(err) = run() {
        eprintln!("{err}");
        process::exit(2);
    }
}

fn run() -> Result<()> {
    let mut args = env::args_os();
    let _exe = args.next();
    let command = next_arg(&mut args, "command")?;

    if command == OsStr::new("--is-folder") {
        let source = next_path(&mut args, "source file")?;
        ensure_no_extra_args(args)?;
        if is_supported_workbook(&source) {
            return Ok(());
        }
        process::exit(1);
    }

    if command == OsStr::new("unpack-folder") {
        let source = next_path(&mut args, "source file")?;
        let destination = next_path(&mut args, "destination folder")?;
        ensure_no_extra_args(args)?;
        return unpack_folder(&source, &destination);
    }

    Err(format!(
        "Usage: excel2tsv --is-folder <workbook> | excel2tsv unpack-folder <workbook> <folder>"
    )
    .into())
}

fn next_arg(args: &mut impl Iterator<Item = OsString>, name: &str) -> Result<OsString> {
    args.next().ok_or_else(|| format!("missing {name}").into())
}

fn next_path(args: &mut impl Iterator<Item = OsString>, name: &str) -> Result<PathBuf> {
    next_arg(args, name).map(PathBuf::from)
}

fn ensure_no_extra_args(mut args: impl Iterator<Item = OsString>) -> Result<()> {
    if let Some(arg) = args.next() {
        return Err(format!("unexpected argument: {}", PathBuf::from(arg).display()).into());
    }
    Ok(())
}

fn is_supported_workbook(path: &Path) -> bool {
    path.extension()
        .and_then(OsStr::to_str)
        .map(|extension| {
            let lower = extension.to_ascii_lowercase();
            SUPPORTED_EXTENSIONS.contains(&lower.as_str())
        })
        .unwrap_or(false)
}

fn unpack_folder(source: &Path, destination: &Path) -> Result<()> {
    if !is_supported_workbook(source) {
        return Err(format!("unsupported Excel workbook: {}", source.display()).into());
    }

    fs::create_dir_all(destination)?;

    let mut workbook = open_workbook_auto(source)?;
    let sheet_names = workbook.sheet_names().to_owned();
    let mut used_names = HashMap::new();

    for sheet_name in sheet_names {
        let range = workbook.worksheet_range(&sheet_name)?;
        let file_name = unique_sheet_file_name(&sheet_name, &mut used_names);
        let destination_file = destination.join(file_name);
        write_range_as_tsv(&range, &destination_file)?;
    }

    Ok(())
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
