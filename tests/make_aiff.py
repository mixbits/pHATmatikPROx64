# Converts a 16-bit WAV file to AIFF (big-endian), for testing the plug-in's AIFF loader.
#   python tests/make_aiff.py loop.wav loop.aif
import struct, sys, wave

with wave.open(sys.argv[1], 'rb') as w:
    channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    data = w.readframes(frames)
assert width == 2, '16-bit input expected'
samples = struct.unpack('<%dh' % (len(data) // 2), data)
big = struct.pack('>%dh' % len(samples), *samples)

def ext80(value):
    # 80-bit IEEE extended, as AIFF stores the sample rate
    exponent = 16383 + 63
    mantissa = int(value)
    while mantissa and not (mantissa >> 63):
        mantissa <<= 1
        exponent -= 1
    return struct.pack('>HQ', exponent, mantissa)

comm = struct.pack('>hIh', channels, frames, 16) + ext80(rate)
ssnd = struct.pack('>II', 0, 0) + big
body = b'AIFF' + b'COMM' + struct.pack('>I', len(comm)) + comm + b'SSND' + struct.pack('>I', len(ssnd)) + ssnd
with open(sys.argv[2], 'wb') as f:
    f.write(b'FORM' + struct.pack('>I', len(body)) + body)
print('wrote', sys.argv[2], frames, 'frames')
