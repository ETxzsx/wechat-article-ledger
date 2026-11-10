# -*- coding: utf-8 -*-
import os
base = r"C:\Users\心玖\Desktop\新建文件夹\推文台账助手\kernel"
parts = ["kernel_part1.h","kernel_part2.h","kernel_part3.h","kernel_part4.h","kernel_part5.h"]
out = []
for p in parts:
    with open(os.path.join(base, p), encoding="utf-8") as f:
        out.append(f.read())
src = "\n".join(out)
src = src.replace("#include <cstdio>", "#include <cstdio>\n#include <cstring>\n#include <windowsx.h>\n#include <memory>", 1)
with open(os.path.join(base, "kernel.cpp"), "w", encoding="utf-8") as f:
    f.write(src)
print("merged", len(src), "chars")
