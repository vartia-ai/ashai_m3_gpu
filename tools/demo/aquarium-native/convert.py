#!/usr/bin/env python3
"""Build the asset pack for aquarium-native from the WebGL Aquarium.

Downloads the aquarium page, script and assets from the WebGLSamples
repository (BSD-3-Clause) into OUTDIR/src, then writes OUTDIR/pack:

  manifest.txt   one record per line (see below)
  NOTICE.txt     the aquarium's BSD-3-Clause notice
  mesh/*.bin     u32 nverts, u32 nidx, f32[14*nverts] (pos3 nrm3 uv2 tan3
                 bin3), u16[nidx]
  tex/*.raw      RGBA8 mip chain, level 0 first
  shaders/*      GLSL 1.20 sources

Shaders are the page's own, put through the same variant selection as its
createProgramFromTags (fog injected, reflection and normal maps on, which
the page switches on once it runs above 40 fps), with the GLSL ES
precision block removed. Needs Python 3 and Pillow.

  convert.py OUTDIR
"""
import json
import os
import re
import struct
import sys
import urllib.request

from PIL import Image

# Pinned upstream revision (2026-10-18), so the pack is reproducible.
REV = "4c87dfea3cff6bf9f351c5ed4a191f86ceed8bbc"
BASE = "https://raw.githubusercontent.com/WebGLSamples/WebGLSamples.github.io/%s/aquarium/" % REV
SKYBOX = ["GlobeOuter_EM_positive_x.jpg", "GlobeOuter_EM_negative_x.jpg",
          "GlobeOuter_EM_positive_y.jpg", "GlobeOuter_EM_negative_y.jpg",
          "GlobeOuter_EM_positive_z.jpg", "GlobeOuter_EM_negative_z.jpg"]

FOG_UNIFORMS = ("uniform float fogPower;\nuniform float fogMult;\n"
                "uniform float fogOffset;\nuniform vec4 fogColor;\n")
FOG_CODE = ("outColor = mix(outColor, vec4(fogColor.rgb, diffuseColor.a),\n"
            "   clamp(pow((v_position.z / v_position.w), fogPower) * fogMult - fogOffset,0.0,1.0));\n")


def fetch(src, rel):
    path = os.path.join(src, rel)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with urllib.request.urlopen(BASE + rel, timeout=60) as r:
            data = r.read()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.rename(path + ".part", path)
    return path


def js_array(text, name):
    """The object-literal array `var NAME = [...]` from aquarium.js, as JSON."""
    m = re.search(r"var %s = (\[.*?\n\]);" % re.escape(name), text, re.S)
    s = m.group(1)
    s = re.sub(r"//[^\n]*", "", s)
    s = s.replace("'", '"')
    s = re.sub(r"([{,]\s*)([A-Za-z_]\w*)\s*:", r'\1"\2":', s)
    s = re.sub(r",(\s*[}\]])", r"\1", s)
    return json.loads(s)


def script(html, sid):
    m = re.search(r'<script id="%s"[^>]*>(.*?)</script>' % re.escape(sid), html, re.S)
    return m.group(1)


def to_glsl120(src):
    out = []
    for line in src.split("\n"):
        t = line.strip()
        if t.startswith("precision ") or t.startswith("#ifdef GL_FRAGMENT_PRECISION_HIGH") \
                or t in ("#else", "#endif"):
            continue
        out.append(line)
    return "#version 120\n" + "\n".join(out).strip() + "\n"


def variant_fs(fs, fog):
    if fog:
        fs = fs.replace("// #fogUniforms", FOG_UNIFORMS)
        fs = fs.replace("// #fogCode", FOG_CODE)
    fs = re.sub(r"(?m)^.*?// #noReflection$", "", fs)
    fs = re.sub(r"(?m)^.*?// #noNormalMap$", "", fs)
    return fs


def pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def write_texture(pack, name, img, flip):
    img = img.convert("RGBA")
    if flip:
        img = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    w, h = img.size
    mips = pow2(w) and pow2(h)
    levels = [img]
    while mips and (levels[-1].size[0] > 1 or levels[-1].size[1] > 1):
        lw, lh = levels[-1].size
        levels.append(levels[-1].resize((max(1, lw // 2), max(1, lh // 2)), Image.Resampling.BOX))
    rel = "tex/%s.raw" % name
    with open(os.path.join(pack, rel), "wb") as f:
        for lv in levels:
            f.write(lv.tobytes())
    return rel, w, h, len(levels), 0 if mips else 1


def main():
    out = sys.argv[1]
    src, pack = os.path.join(out, "src"), os.path.join(out, "pack")
    for d in ("mesh", "tex", "shaders"):
        os.makedirs(os.path.join(pack, d), exist_ok=True)
    js = open(fetch(src, "aquarium.js")).read()
    html = open(fetch(src, "aquarium.html")).read()
    notice = re.search(r"<!--(.*?)-->", html, re.S).group(1)
    with open(os.path.join(pack, "NOTICE.txt"), "w") as f:
        body = "\n".join(re.sub(r"^\s*\* ?", "", l) for l in notice.strip().split("\n"))
        f.write("WebGL Aquarium, WebGLSamples revision %s\n\n%s\n" % (REV, body.strip()))
    scenes = js_array(js, "g_sceneInfo")
    fish = js_array(js, "g_fishTable")
    placement = json.load(open(fetch(src, "assets/PropPlacement.js")))["objects"]

    lines = []
    textures = {}
    programs = {}

    def texture(fname, flip=True):
        if fname not in textures:
            img = Image.open(fetch(src, "assets/" + fname))
            key = os.path.splitext(fname)[0]
            rel, w, h, n, clamp = write_texture(pack, key, img, flip)
            lines.append("tex %s %s %d %d %d %d" % (key, rel, w, h, n, clamp))
            textures[fname] = key
        return textures[fname]

    def program(vs_id, fs_id, fog):
        key = "%s+%s%s" % (vs_id, fs_id, "+fog" if fog else "")
        if key not in programs:
            name = "p%d" % len(programs)
            vs = to_glsl120(script(html, vs_id))
            fs = to_glsl120(variant_fs(script(html, fs_id), fog))
            for ext, text in (("vert", vs), ("frag", fs)):
                with open(os.path.join(pack, "shaders/%s.%s" % (name, ext)), "w") as f:
                    f.write(text)
            lines.append("prog %s shaders/%s.vert shaders/%s.frag %s" % (name, name, name, key))
            programs[key] = name
        return programs[key]

    # The reflection cube map (not flipped, as tdl loads cube faces).
    faces = []
    for fname in SKYBOX:
        key = os.path.splitext(fname)[0]
        rel, w, h, n, _ = write_texture(pack, key, Image.open(fetch(src, "assets/" + fname)), False)
        faces.append(rel)
    lines.append("cube skybox %d %d %s" % (w, n, " ".join(faces)))

    for info in scenes:
        name = info["name"]
        data = json.load(open(fetch(src, "assets/%s.js" % name)))
        fog = info.get("fog", True)
        for mi, model in enumerate(data["models"]):
            tex = model["textures"]
            if "program" in info:
                vs_id, fs_id = info["program"]
            elif "reflectionMap" in tex:
                vs_id, fs_id = "reflectionMapVertexShader", "reflectionMapFragmentShader"
            elif "normalMap" in tex:
                vs_id, fs_id = "normalMapVertexShader", "normalMapFragmentShader"
            else:
                vs_id, fs_id = "diffuseVertexShader", "diffuseFragmentShader"
            prog = program(vs_id, fs_id, fog)
            f = model["fields"]
            nv = len(f["position"]["data"]) // 3
            def field(k, n):
                d = f.get(k, {}).get("data")
                return d if d else [0.0] * (n * nv)
            pos, nrm, uv = field("position", 3), field("normal", 3), field("texCoord", 2)
            tan, bin_ = field("tangent", 3), field("binormal", 3)
            idx = f["indices"]["data"]
            rel = "mesh/%s_%d.bin" % (name, mi)
            with open(os.path.join(pack, rel), "wb") as fo:
                fo.write(struct.pack("<II", nv, len(idx)))
                vals = []
                for i in range(nv):
                    vals += pos[3*i:3*i+3] + nrm[3*i:3*i+3] + uv[2*i:2*i+2] + tan[3*i:3*i+3] + bin_[3*i:3*i+3]
                fo.write(struct.pack("<%df" % len(vals), *vals))
                fo.write(struct.pack("<%dH" % len(idx), *idx))
            maps = [texture(tex[k]) if k in tex else "-" for k in ("diffuse", "normalMap", "reflectionMap")]
            lines.append("model %s %s %d %d %s %s %s" % (
                name, prog, 1 if fog else 0, 1 if info.get("blend") else 0,
                info.get("group", "base"), rel, " ".join(maps)))

    lr_prog = program("texVertexShader", "texFragmentShader", False)
    lines.append("lightray %s %s" % (lr_prog, texture("LightRay.png", flip=False)))

    # The page's camera presets ("Change View"); it starts in the first.
    common = open(fetch(src, "aquarium-common.js")).read()
    for v in js_array(common, "g_viewSettings"):
        lines.append("view " + " ".join("%g" % v[k] for k in (
            "eyeHeight", "eyeRadius", "eyeSpeed", "targetHeight", "targetRadius", "fieldOfView",
            "ambientRed", "ambientGreen", "ambientBlue", "fogPower", "fogMult", "fogOffset",
            "fogRed", "fogGreen", "fogBlue")))

    for fi in fish:
        c = fi["constUniforms"]
        lines.append("fish %s %g %g %g %g %g %g %g %g %g %g" % (
            fi["name"], fi["speed"], fi["speedRange"], fi["radius"], fi["radiusRange"],
            fi["tailSpeed"], fi["heightOffset"], fi["heightRange"],
            c["fishLength"], c["fishWaveLength"], c["fishBendAmount"]))

    for obj in placement:
        lines.append("place %s %s" % (obj["name"], " ".join("%g" % v for v in obj["worldMatrix"])))

    with open(os.path.join(pack, "manifest.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("pack: %d programs, %d textures, %d placements -> %s" % (
        len(programs), len(textures) + 6, len(placement), pack))


if __name__ == "__main__":
    main()
