"""Helper shared by the mut_mNN.py scripts: applies or reverts one textual mutation of a source file.

The pattern must match exactly once; CRLF sources are handled; the original is kept as <file>.mutbak
while a mutation is applied.
"""
import io
import os


def run(src, old, new, mode):
    """Applies (or reverts) one mutation. Patterns are written with LF; they are converted when the
    source file uses CRLF line endings (the working tree does, because of core.autocrlf)."""
    backup = src + ".mutbak"
    if mode == "apply":
        text = io.open(src, encoding="utf-8", newline="").read()
        if "\r\n" in text:
            old = old.replace("\r\n", "\n").replace("\n", "\r\n")
            new = new.replace("\r\n", "\n").replace("\n", "\r\n")
        count = text.count(old)
        if count != 1:
            raise SystemExit("[mut] pattern found %d times in %s" % (count, src))
        io.open(backup, "w", encoding="utf-8", newline="").write(text)
        io.open(src, "w", encoding="utf-8", newline="").write(text.replace(old, new))
    else:
        data = io.open(backup, encoding="utf-8", newline="").read()
        io.open(src, "w", encoding="utf-8", newline="").write(data)
        os.remove(backup)
