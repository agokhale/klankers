# Agent Rules and Preferences

## Output and Code Formatting
- **Never omit lines**: Do not truncate, summarize away, collapse, or omit lines in command outputs, code blocks, diffs, or file listings.
- **No placeholders**: Never use placeholders such as `...`, `/* existing code unchanged */`, or `[rest of output omitted]`. Always provide full, complete code and output verbatim.
- **Command Transparency**: Avoid compact multi-line heredocs (`<< 'EOF'`) that get collapsed with `... (lines omitted)` in UI approval dialogs. When running commands, keep them single-line or write the code directly to a file before invoking tools so that all lines remain visible for inspection.
