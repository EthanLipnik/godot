# NVIDIA STBN packed runtime asset

This directory contains a generated runtime pack of four canonical NVIDIA
Spatiotemporal Blue Noise scalar components. The pack is built offline from
the official NVIDIA-RTX/STBN repository at commit
`48b2839e4d8b7f0202ac72c6b0ae720d235a5b8b`, source archive
`Assets/STBN.zip` (SHA-256:
`f262aaa79704b913ad1ac22b11674931c5c16a788688f8ce49ec43d59eb5c747`).

The four packed channels are the scalar `2Dx1Dx1D` field, the red component
of `vec1`, and the red/green components of `vec2`. All are 128x128x64
canonical STBN fields. The generated RGBA8 array is 4,194,304 bytes and is
uploaded as one 128x128x64 texture array. Re-run `generate_packed_stbn.py`
only from the pinned extracted archive; runtime never reads PNGs or accesses
the network. The packed byte stream SHA-256 is
`f705d23e410846185b4966957f147bdd376720ff383f2d141d02c05be30b8c23`; the
engine metadata/data FNV checksum is `0x37731ac2531d3b4a`.

The source data is redistributed under the complete NVIDIA license in
`License.txt`. The packed file is a generated derivative and remains subject
to that license and its use restrictions.
