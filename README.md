# cat2 — Intelligent Auto-Detecting Codepage Converter & Terminal Viewer

`cat2` is a high-performance stream and file processor designed for cross-platform z/OS and Unix development. It automatically detects and converts mixed ASCII, EBCDIC (IBM-1047), and double-converted text streams into clean, human-readable terminal output.

## Features

- **Multi-Candidate Sliding Window**: Evaluates candidate conversions in parallel (ASCII, EBCDIC-1047, UTF-8, double-converted ASCII/EBCDIC, fallback).
- **UTF-8 Aware**: Accurately recognizes multi-byte UTF-8 sequences without treating high bytes as invalid.
- **Dynamic Character Quality Scoring**: Breaks ties on ambiguous short lines using character preference weights (alphanumeric > whitespace > punctuation > control).
- **Flexible Codepage Output**: Output in ASCII (`-a`, default on Linux/macOS) or EBCDIC (`-e`, default on z/OS).
- **Streaming & Multi-file Support**: Reads standard input or any number of files with `-` for stdin interleaving.
- **Built-in Model Context Protocol (MCP) Server**: Provides `cat2-mcp` for direct integration into AI agents and developer harnesses.

## Building

```bash
cd cat2
make
```

To run tests:
```bash
make test
```

## CLI Usage

```bash
# View a file with automatic encoding detection
cat2 /path/to/logfile

# Pipe mixed remote logs to cat2
ssh user@zos 'cat /path/to/build.log' | cat2

# Force ASCII output
cat2 -a build.log

# Force EBCDIC output (e.g. when redirecting on z/OS)
cat2 -e build.log

# Save raw input to a separate file while viewing
cat2 -o raw.bin mixed_input.txt
```

## MCP Server Integration

`cat2-mcp` runs over stdio (JSON-RPC 2.0).

### Tools Exposed

- `cat2_read`: Read a file and return decoded text with encoding breakdown.
- `cat2_detect`: Analyze a file and report detected encoding metrics (ASCII, EBCDIC, UTF-8, binary).
- `cat2_convert`: Convert text/data between ASCII, EBCDIC, and auto-detected modes.
- `cat2_help`: Display usage and technical details.

### Registration in `.bob/mcp.json`

```json
{
  "mcpServers": {
    "cat2": {
      "command": "/path/to/cat2/cat2-mcp",
      "args": []
    }
  }
}
```
