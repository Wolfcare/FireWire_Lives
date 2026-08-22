#!/usr/bin/env python3
"""
extract_pdf_text.py - pull text out of a PDF with no third-party dependencies.

Written because the 1394 OHCI specification is distributed as a PDF and this
machine has no poppler, mutool or pyobjc. Handles the two stream filters the
specification actually uses, FlateDecode and LZWDecode, and recovers the text
showing operators. Layout is not preserved - this is for grepping a spec, not
for reproducing it.

  usage: extract_pdf_text.py <file.pdf> [> out.txt]
"""

import re
import sys
import zlib


def lzw_decode(buf: bytes) -> bytes:
    """PDF LZWDecode: variable 9-12 bit codes, 256 = clear, 257 = end of data."""
    def fresh():
        return {i: bytes([i]) for i in range(256)}, 258, 9

    out = bytearray()
    table, nxt, width = fresh()
    prev = None
    bitbuf = bitcnt = 0

    for byte in buf:
        bitbuf = (bitbuf << 8) | byte
        bitcnt += 8
        while bitcnt >= width:
            code = (bitbuf >> (bitcnt - width)) & ((1 << width) - 1)
            bitcnt -= width

            if code == 256:                      # clear table
                table, nxt, width = fresh()
                prev = None
                continue
            if code == 257:                      # end of data
                return bytes(out)

            if prev is None:
                entry = table.get(code, b"")
            elif code in table:
                entry = table[code]
            else:                                # the KwKwK case
                entry = table[prev] + table[prev][:1]

            out += entry
            if prev is not None:
                table[nxt] = table[prev] + entry[:1]
                nxt += 1
                # Widen one code early: the encoder switches on the next code.
                if nxt + 1 >= (1 << width) and width < 12:
                    width += 1
            prev = code

    return bytes(out)


def extract(path: str) -> str:
    data = open(path, "rb").read()
    pieces = []

    for m in re.finditer(rb"<<(.*?)>>\s*stream\r?\n", data, re.S):
        header = m.group(1)
        body = data[m.end():data.find(b"endstream", m.end())]

        try:
            if b"FlateDecode" in header:
                decoded = zlib.decompress(body)
            elif b"LZWDecode" in header:
                decoded = lzw_decode(body)
            else:
                continue
        except Exception:
            continue                              # not every stream is text

        # Text-showing operators carry their strings in parentheses.
        chunk = []
        for tok in re.findall(rb"\((?:\\.|[^\\()])*\)", decoded):
            s = re.sub(rb"\\([()\\])", rb"\1", tok[1:-1])
            chunk.append(s.decode("latin-1"))
        if chunk:
            pieces.append("".join(chunk))

    return "\n".join(pieces)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.stdout.write(extract(sys.argv[1]))
