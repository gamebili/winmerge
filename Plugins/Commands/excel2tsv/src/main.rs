use std::env;
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;
use std::process;

type Result<T> = excel2tsv_dll::Result<T>;

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
        if excel2tsv_dll::is_supported_workbook(&source) {
            return Ok(());
        }
        process::exit(1);
    }

    if command == OsStr::new("unpack-folder") {
        let source = next_path(&mut args, "source file")?;
        let destination = next_path(&mut args, "destination folder")?;
        ensure_no_extra_args(args)?;
        let diagnostics = excel2tsv_dll::unpack_folder_with_diagnostics(
            &source,
            &destination,
            excel2tsv_dll::excel_log_enabled(),
        )?;
        if !diagnostics.is_empty() {
            eprint!("{diagnostics}");
        }
        return Ok(());
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
