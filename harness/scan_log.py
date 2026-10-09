#!/usr/bin/env python3
import re, sys
from collections import Counter

# Accepts several logs (kyty.log, guest-printf.log); LOGF output lives in the latter.
import os
text = "\n".join(open(f, errors="replace").read() for f in sys.argv[1:] if os.path.exists(f))
patterns = {
    "unresolved_import": r"(?i)(?:unresolved|cannot resolve|symbol not found|missing import).{0,180}",
    "unimplemented": r"(?i)(?:not implemented|unimplemented|unsupported syscall|unsupported function).{0,180}",
    "unsupported_instruction": r"(?i)(?:unsupported instruction|illegal instruction|unknown opcode).{0,180}",
    "gpu_validation": r"(?i)(?:validation|vulkan|spir-v|shader).{0,180}(?:error|failed|unsupported|invalid).{0,180}",
    "crash": r"(?i)(?:crash|exception|segmentation fault|access violation|abort|fatal).{0,180}",
}
for name, pattern in patterns.items():
    hits = [re.sub(r"\s+", " ", m.group(0)).strip() for m in re.finditer(pattern, text)]
    counts = Counter(hits)
    print("[%s] %d matches" % (name, len(hits)))
    for line, count in counts.most_common(10):
        print("  %dx %s" % (count, line))

frames = re.findall(r"FrameStats: present=(\d+) (\d+)x(\d+) avg=([\d.]+) max=(\d+) nonblack=([\d.]+)% (\w+)", text)
black = sum(1 for f in frames if f[6] == "ALL_BLACK")
print("[frames] %d sampled, %d all-black, %d with content" % (len(frames), black, len(frames) - black))
