# License

This repository contains two kinds of material under two different terms.

## 1. Modifications to CRYENGINE (Crytek copyright)

Everything under `engine/Code/CryEngine/`, `engine/Engine/`, `engine/Tools/` and every diff in
`patches/` that touches those paths is a modification of CRYENGINE source code.

CRYENGINE is Copyright (c) Crytek GmbH. All rights reserved. It is licensed, not sold, under
the CRYENGINE Limited License Agreement (https://www.cryengine.com/ce-terms). These files are
provided only as changes to be applied by users who hold their own CRYENGINE license and have
obtained the CRYENGINE 5.7.1 source code from Crytek under that agreement. No part of the
CRYENGINE engine itself is distributed here, and nothing in this repository grants any right to
CRYENGINE beyond what your own agreement with Crytek grants you. If you do not have a CRYENGINE
license, obtain one from Crytek before using this material.

## 2. Original work of the ReC Sandbox project (MIT)

Everything else - the `CinematicCamera` and `CryPhoneTracker` plugins under
`engine/Code/CryPlugins/`, the documentation in `docs/`, the design specifications, the sync
tooling and this repository's own files - is original work of the ReC Sandbox project and is
released under the MIT License:

```
MIT License

Copyright (c) 2026 Sherefox

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The plugins include CRYENGINE headers and link against CRYENGINE; building and running them
requires a CRYENGINE license as described in section 1. "CRYENGINE" and "Crytek" are
trademarks of Crytek GmbH; this project is not affiliated with or endorsed by Crytek.
