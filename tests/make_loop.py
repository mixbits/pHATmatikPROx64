# Writes a deterministic two-bar drum loop (120 bpm, 44.1 kHz, 16-bit stereo) for loading into
# the plug-in during tests.   python tests/make_loop.py build/test/loop.wav
import math, struct, sys

rate = 44100
beat = rate // 2            # 120 bpm
total = beat * 8
out = [0.0] * total
seed = 12345

def noise():
    global seed
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
    return seed / 0x3FFFFFFF - 1.0

def add(start, length, fn):
    for i in range(length):
        if start + i < total:
            out[start + i] += fn(i)

def kick(i):
    t = i / rate
    return math.sin(2 * math.pi * (50 * t + 60 * (1 - math.exp(-t * 30)) / 30)) * math.exp(-t * 9) * 0.9

def snare(i):
    t = i / rate
    return (noise() * 0.6 + math.sin(2 * math.pi * 190 * t) * 0.4) * math.exp(-t * 18) * 0.7

def hat(i):
    return noise() * math.exp(-i / rate * 70) * 0.35

for b in range(8):
    if b % 4 in (0, 2): add(b * beat, beat // 2, kick)
    if b % 4 in (1, 3): add(b * beat, beat // 2, snare)
    add(b * beat, beat // 6, hat)
    add(b * beat + beat // 2, beat // 6, hat)
add(3 * beat + beat // 2 + beat // 4, beat // 4, kick)
add(7 * beat + beat // 2, beat // 4, snare)
add(7 * beat + 3 * beat // 4, beat // 4, snare)

frames = b''.join(struct.pack('<hh', *(2 * [max(-32767, min(32767, int(v * 30000)))])) for v in out)
with open(sys.argv[1], 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + len(frames)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16))
    f.write(b'data' + struct.pack('<I', len(frames)) + frames)
print('wrote', sys.argv[1], total / rate, 'seconds')
