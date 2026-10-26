"""Digest session logs and launcher crash reports (Python 3.12, stdlib only).

Folders are searched recursively for .log and .txt reports. Line numbers refer
to the submitted file, including launcher headers. Launcher (xN) repeats count
as N occurrences. Context keeps the first two distinct lines and the latest;
problem examples keep their first line and up to eight distinct +0x addresses.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys


def redact(text: str) -> str:
    return re.sub(r"C:[\\/]Users[\\/][^\\/\r\n]+[\\/]",
                  lambda m: "C:\\Users\\<user>\\", text, flags=re.IGNORECASE)


# Strings from probe.c, runtime_memory.c, win32_memory.c, bbport_threads.h,
# coop_hooks.cpp, party_link.cpp and the Vulkan renderer. Do not classify the
# optional, caught "C++ exception, thread ..." trace as an uncaught exception.
PATTERNS = {
    "crash": re.compile(r"\b(?:Guest|Host) fault\b|\bFault \(signal|\bSTOP:"),
    "cpp_exception": re.compile(r"uncaught C\+\+ exception", re.IGNORECASE),
    "vulkan": re.compile(r"\bVK_ERROR\w*|\bdevice lost\b|\bErrorDeviceLost\b", re.IGNORECASE),
    "memory": re.compile(r"Runtime: (?:no room\b|committing .* failed\b)", re.IGNORECASE),
    "party_link": re.compile(
        r"(?:Party: link:|party link:)\s*(?:lost .+ \(slot \d+\):|reconnecting:|rejected:|"
        r"stopped: dropped\b|.*\b(?:timed out|timeout|no traffic|disconnected)\b)",
        re.IGNORECASE),
    "coop_hooks": re.compile(r"Coop hooks:.*\b(?:not installed|not written|not writable)\b"),
}
FIELDS = ("build", "gpu", "vram", "role", "link", "link_type", "nat")


def remember(items: list, number: int, text: str) -> None:
    if any(item["text"] == text for item in items):
        return
    entry = {"line": number, "text": text}
    if len(items) < 3:
        items.append(entry)
    else:
        items[-1] = entry


def digest(path: Path) -> dict:
    result = {"file": redact(str(path)), "context": {key: [] for key in FIELDS},
              "problems": {}}
    # Replacement decoding tolerates truncated UTF-8 from a crashed process.
    with path.open(encoding="utf-8-sig", errors="replace") as stream:
        for number, raw in enumerate(stream, 1):
            line = redact(raw.strip())
            folded = re.search(r"  \(x([1-9][0-9]*)\)$", line)
            count = int(folded[1]) if folded else 1
            if folded:
                line = line[:folded.start()]
            context = result["context"]
            if "Nightwatch Co-op" in line or "visy v" in line or "party build" in line.lower():
                remember(context["build"], number, line)
            if ("GPU:" in line and "driver " in line and "Vulkan " in line
                    or "GB RAM, Windows " in line):
                remember(context["gpu"], number, line)
            if line.startswith("GPU memory:"):
                remember(context["vram"], number, line)
            if "party networking on (" in line or re.search(r"Party: (?:host|guest) ", line):
                remember(context["role"], number, line)
            if re.search(r"Party: link:|party link:|network path:", line, re.IGNORECASE):
                remember(context["link"], number, line)
            if "network path: party link over " in line or "connected over UDP" in line:
                remember(context["link_type"], number, line)
            if "Party: NAT:" in line:
                remember(context["nat"], number, line)

            categories = [key for key, pattern in PATTERNS.items() if pattern.search(line)]
            # CrashReport.cs headers may be all that remains of a short report.
            exit_code = re.match(r"Exit code 0x([0-9a-fA-F]+):", line)
            if exit_code and int(exit_code[1], 16) not in (0, 75) and "crash" not in categories:
                categories.append("crash")
            for category in categories:
                problem = result["problems"].setdefault(category, {
                    "first_line": number, "count": 0, "example": line, "addresses": []})
                problem["count"] += count
                for address in re.findall(r"\+0x[0-9a-fA-F]+|guest offset 0x[0-9a-fA-F]+", line):
                    address = address.replace("guest offset 0x", "+0x").lower()
                    if address not in problem["addresses"] and len(problem["addresses"]) < 8:
                        problem["addresses"].append(address)
    return result


def summarize(reports: list) -> dict:
    counts = Counter(category for report in reports for category in report["problems"])
    return {"files": len(reports), "categories": dict(sorted(
        counts.items(), key=lambda item: (-item[1], item[0])))}


def print_digest(report: dict) -> None:
    print(report["file"])
    for field, entries in report["context"].items():
        for entry in entries:
            print(f"  {field} L{entry['line']}: {entry['text']}")
    if not report["problems"]:
        print("  Problems: none detected")
    for category, problem in report["problems"].items():
        addresses = ", ".join(problem["addresses"])
        suffix = f" [{addresses}]" if addresses else ""
        print(f"  {category}: {problem['count']} hits, first L{problem['first_line']}{suffix}")
        print(f"    {problem['example']}")


def main(argv=None) -> int:
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            super().error(redact(message))

    parser = Parser(prog="log_digest.py", description=__doc__)
    parser.add_argument("paths", nargs="+", help="log/report files or folders")
    parser.add_argument("--json", action="store_true", help="emit JSON")
    parser.add_argument("--summary", action="store_true", help="count files per category")
    args = parser.parse_args(argv)
    files = {}
    errors = []
    for name in args.paths:
        path = Path(name)
        try:
            if path.is_dir():
                found = sorted(p for p in path.rglob("*")
                               if p.suffix.lower() in (".log", ".txt") and p.is_file())
                if not found:
                    errors.append(redact(f"No .log or .txt reports in {path}"))
            else:
                found = [path]
            for candidate in found:
                files[candidate.resolve()] = candidate
        except OSError as exc:
            errors.append(redact(str(exc)))
    reports = []
    for path in files.values():
        try:
            reports.append(digest(path))
        except OSError as exc:
            errors.append(redact(str(exc)))
    if args.json:
        output = summarize(reports) if args.summary else {"reports": reports}
        output["errors"] = errors
        print(json.dumps(output, indent=2, ensure_ascii=False))
    else:
        if args.summary:
            summary = summarize(reports)
            print(f"Files: {summary['files']}")
            print("Category          Files")
            for category, count in summary["categories"].items():
                print(f"{category:<18}{count}")
        else:
            for report in reports:
                print_digest(report)
        for error in errors:
            print(f"Error: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
