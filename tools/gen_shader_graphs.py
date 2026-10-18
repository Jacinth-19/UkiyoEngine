#!/usr/bin/env python3
"""Generates the Ukiyo shader-graph sources (50 shaders) as JSON.

Each output is tools/shader_graphs/<Name>.json with a "graph" object in the editor's format.
ukiyo_shader_tool compile tools/shader_graphs assets compiles them into assets/Shaders/<Name>.ukshader.

Node library limits (sg_nodes.cpp): no noise, sine, fract or dot-product nodes, so the effects use
height gradients, thresholds, smoothstep edges and the world normal's Y component instead.
"""
import json
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shader_graphs")


class Graph:
    def __init__(self):
        self.nodes = []
        self.conns = []

    def add(self, model, params=(), ref=""):
        nid = len(self.nodes)
        col = nid % 6
        row = nid // 6
        self.nodes.append({
            "id": nid,
            "internal-data": {"model-name": model, "params": [str(p) for p in params], "reference": ref},
            "label": "",
            "labelVisible": True,
            "position": {"x": col * 220, "y": row * 120},
        })
        return nid

    def link(self, src, sp, dst, dp):
        self.conns.append({"inNodeId": dst, "inPortIndex": dp, "outNodeId": src, "outPortIndex": sp})

    def f(self, value, ref=""):
        """Float constant or exposed property (ref names the material property)."""
        return Ref(self.add("Float", [value], ref))

    def col(self, rgb_values, ref=""):
        """Colour constant or exposed property."""
        return Ref(self.add("Color", rgb_values, ref))

    def to_json(self):
        return {"graph": {"connections": self.conns, "nodes": self.nodes}}


class Ref:
    """A node output (port 0 unless .port is set)."""

    def __init__(self, nid, port=0):
        self.id = nid
        self.port = port

    def __getitem__(self, port):
        return Ref(self.id, port)


def _in_port(g, dst, idx, v):
    if isinstance(v, Ref):
        g.link(v.id, v.port, dst, idx)
    elif isinstance(v, tuple):
        g.link(v[0], v[1], dst, idx)
    else:
        c = g.add("Float", [v])
        g.link(c, 0, dst, idx)


def op(g, model, *ins):
    nid = g.add(model)
    for i, v in enumerate(ins):
        _in_port(g, nid, i, v)
    return Ref(nid)


def master(g, model, *ins):
    nid = g.add(model)
    for i, v in enumerate(ins):
        if v is None:
            continue
        _in_port(g, nid, i, v)
    return nid


def add(g, a, b): return op(g, "AddF", a, b)
def mul(g, a, b): return op(g, "MultiplyF", a, b)
def neg(g, a): return op(g, "MultiplyF", a, -1.0)
def one_minus(g, a): return op(g, "OneMinusF", a)
def sat(g, a): return op(g, "SaturateF", a)
def ss(g, e0, e1, x): return op(g, "SmoothstepF", e0, e1, x)
def pw(g, a, e): return op(g, "PowerF", a, e)
def lerp_v(g, a, b, t): return op(g, "LerpV3", a, b, t)
def mul_v(g, a, b): return op(g, "MultiplyV3", a, b)
def add_v(g, a, b): return op(g, "AddV3", a, b)
def comb(g, r, gg, b): return op(g, "Combine", r, gg, b)
def splat(g, s): return op(g, "Combine", s, s, s)
def world_comp(g, comp):
    """World position component: 0 = X, 1 = Y, 2 = Z."""
    w = Ref(g.add("WorldPosition"))
    sp = g.add("Split")
    g.link(w.id, 0, sp, 0)
    return Ref(sp, comp)


def normal_comp(g, comp):
    n = Ref(g.add("Normal"))
    sp = g.add("Split")
    g.link(n.id, 0, sp, 0)
    return Ref(sp, comp)


def time(g):
    return Ref(g.add("Time"))


# ---------------------------------------------------------------- recipes

def dissolve(base, glow, timed=False, speed=0.25):
    """PBR dissolve: a threshold sweeps down world Y; a bright band burns along the front."""
    def build():
        g = Graph()
        y = world_comp(g, 1)
        if timed:
            thr = sat(g, mul(g, time(g), g.f(speed, "speed")))
        else:
            thr = g.f(0.0, "dissolve")
        # d > 0 is kept, d < 0 is dissolved. Range: thr 0 keeps the whole object, thr 1 removes it.
        d = add(g, add(g, y, g.f(1.5)), mul(g, thr, g.f(-3.0)))
        edge = g.f(0.08, "edgeWidth")
        a = ss(g, neg(g, edge), g.f(0.0), d)     # 0 below the front, 1 above
        b = ss(g, g.f(0.0), edge, d)             # 0 at and below the front, 1 above it
        front = add(g, a, neg(g, b))             # bright band around d = 0
        emis = mul_v(g, g.col(glow, "glowColor"), splat(g, mul(g, front, g.f(3.0, "glowStrength"))))
        master(g, "PBRMaster", g.col(base, "baseColor"), g.f(0.0, "metallic"), g.f(0.4, "smoothness"),
               None, emis, a)
        return g
    return build


def hologram(base, fade_lo=-0.5, fade_hi=1.0):
    def build():
        g = Graph()
        y = world_comp(g, 1)
        fade = ss(g, g.f(fade_lo, "fadeBottom"), g.f(fade_hi, "fadeTop"), y)
        ny = normal_comp(g, 1)
        rim = pw(g, one_minus(g, mul(g, ny, ny)), g.f(2.0, "rimPower"))
        alpha = mul(g, add(g, g.f(0.15), mul(g, rim, g.f(0.85))), fade)
        color = add_v(g, g.col(base, "baseColor"), mul_v(g, g.col(base, "baseColor"), splat(g, rim)))
        master(g, "UnlitMaster", color, alpha)
        return g
    return build


def toon(base, shadow):
    def build():
        g = Graph()
        ny = normal_comp(g, 1)
        s = ss(g, g.f(-0.05, "edgeLow"), g.f(0.05, "edgeHigh"), ny)
        tone = lerp_v(g, g.col(shadow, "shadowColor"), comb(g, g.f(1.0), g.f(1.0), g.f(1.0)), s)
        master(g, "PBRMaster", mul_v(g, g.col(base, "baseColor"), tone), g.f(0.0, "metallic"),
               g.f(0.15, "smoothness"))
        return g
    return build


def gradient(bottom, top):
    def build():
        g = Graph()
        ny = normal_comp(g, 1)
        s = ss(g, g.f(-1.0), g.f(1.0), ny)
        master(g, "UnlitMaster", lerp_v(g, g.col(bottom, "bottomColor"), g.col(top, "topColor"), s),
               g.f(1.0, "alpha"))
        return g
    return build


def height_fade(color, lo=0.0, hi=1.5):
    def build():
        g = Graph()
        y = world_comp(g, 1)
        alpha = ss(g, g.f(lo, "fadeBottom"), g.f(hi, "fadeTop"), y)
        master(g, "UnlitMaster", g.col(color, "color"), alpha)
        return g
    return build


def glow(base, emis, strength=2.0):
    def build():
        g = Graph()
        master(g, "PBRMaster", g.col(base, "baseColor"), g.f(0.0, "metallic"), g.f(0.3, "smoothness"),
               None, mul_v(g, g.col(emis, "emissionColor"), splat(g, g.f(strength, "strength"))))
        return g
    return build


def metal(base, smooth):
    def build():
        g = Graph()
        master(g, "PBRMaster", g.col(base, "baseColor"), g.f(1.0, "metallic"), g.f(smooth, "smoothness"))
        return g
    return build


def plastic(base, metallic, smooth):
    def build():
        g = Graph()
        master(g, "PBRMaster", g.col(base, "baseColor"), g.f(metallic, "metallic"), g.f(smooth, "smoothness"))
        return g
    return build


def glass(tint, alpha, smooth=0.95):
    def build():
        g = Graph()
        master(g, "PBRMaster", g.col(tint, "tint"), g.f(0.0, "metallic"), g.f(smooth, "smoothness"),
               None, None, g.f(alpha, "alpha"))
        return g
    return build


def force_field(color, power=3.0, peak=0.9):
    def build():
        g = Graph()
        ny = normal_comp(g, 1)
        fres = pw(g, one_minus(g, mul(g, ny, ny)), g.f(power, "falloff"))
        alpha = mul(g, fres, g.f(peak, "peakAlpha"))
        master(g, "UnlitMaster", g.col(color, "color"), alpha)
        return g
    return build


def xray(color):
    def build():
        g = Graph()
        ny = normal_comp(g, 1)
        fres = pw(g, one_minus(g, mul(g, ny, ny)), g.f(2.0, "falloff"))
        alpha = add(g, g.f(0.08, "baseAlpha"), mul(g, fres, g.f(0.45, "rimAlpha")))
        master(g, "UnlitMaster", g.col(color, "color"), alpha)
        return g
    return build


def flat(color):
    def build():
        g = Graph()
        master(g, "UnlitMaster", g.col(color, "color"), g.f(1.0))
        return g
    return build


# ---------------------------------------------------------------- the 50 shaders

SHADERS = {}

FX = {  # name: (base, glow)
    "Fire": ((0.25, 0.05, 0.02), (1.0, 0.45, 0.05)),
    "Ice": ((0.6, 0.85, 1.0), (0.4, 0.9, 1.0)),
    "Toxic": ((0.1, 0.2, 0.05), (0.5, 1.0, 0.1)),
    "Gold": ((1.0, 0.75, 0.3), (1.0, 0.9, 0.4)),
    "Void": ((0.05, 0.02, 0.08), (0.6, 0.2, 1.0)),
    "Electric": ((0.05, 0.1, 0.3), (0.3, 0.7, 1.0)),
    "Blood": ((0.2, 0.01, 0.01), (0.9, 0.02, 0.05)),
    "Sakura": ((1.0, 0.8, 0.85), (1.0, 0.5, 0.7)),
}
for k, (b, gl) in FX.items():
    SHADERS[f"Dissolve_{k}"] = dissolve(b, gl)
for k in ("Fire", "Ice", "Toxic", "Void"):
    b, gl = FX[k]
    SHADERS[f"DissolveTimed_{k}"] = dissolve(b, gl, timed=True)

SHADERS["Hologram_Cyan"] = hologram((0.2, 0.9, 1.0))
SHADERS["Hologram_Green"] = hologram((0.3, 1.0, 0.4))
SHADERS["Hologram_Amber"] = hologram((1.0, 0.7, 0.2))
SHADERS["Hologram_Red"] = hologram((1.0, 0.2, 0.25))

SHADERS["Toon_White"] = toon((0.92, 0.92, 0.95), (0.6, 0.62, 0.72))
SHADERS["Toon_Skin"] = toon((1.0, 0.82, 0.72), (0.78, 0.55, 0.55))
SHADERS["Toon_Red"] = toon((0.9, 0.2, 0.2), (0.45, 0.1, 0.18))
SHADERS["Toon_Green"] = toon((0.3, 0.8, 0.35), (0.12, 0.4, 0.3))
SHADERS["Toon_Blue"] = toon((0.25, 0.5, 0.95), (0.1, 0.2, 0.5))

SHADERS["Gradient_Sunset"] = gradient((0.9, 0.3, 0.2), (1.0, 0.8, 0.4))
SHADERS["Gradient_Ocean"] = gradient((0.02, 0.1, 0.25), (0.3, 0.8, 0.9))
SHADERS["Gradient_Forest"] = gradient((0.05, 0.2, 0.05), (0.5, 0.8, 0.3))
SHADERS["Gradient_Night"] = gradient((0.02, 0.02, 0.08), (0.2, 0.15, 0.4))
SHADERS["Gradient_Mono"] = gradient((0.1, 0.1, 0.1), (0.9, 0.9, 0.9))

SHADERS["HeightFade_White"] = height_fade((1.0, 1.0, 1.0))
SHADERS["HeightFade_Blue"] = height_fade((0.3, 0.6, 1.0))
SHADERS["HeightFade_Dark"] = height_fade((0.05, 0.05, 0.08))

SHADERS["Glow_Lava"] = glow((0.08, 0.02, 0.01), (1.0, 0.35, 0.05))
SHADERS["Glow_Crystal"] = glow((0.2, 0.15, 0.3), (0.6, 0.3, 1.0))
SHADERS["Glow_NeonPink"] = glow((0.05, 0.02, 0.05), (1.0, 0.2, 0.7))
SHADERS["Glow_NeonBlue"] = glow((0.02, 0.03, 0.06), (0.2, 0.6, 1.0))

SHADERS["Metal_Chrome"] = metal((0.8, 0.8, 0.82), 0.95)
SHADERS["Metal_Gold"] = metal((1.0, 0.76, 0.34), 0.85)
SHADERS["Metal_Copper"] = metal((0.95, 0.55, 0.35), 0.7)
SHADERS["Metal_Silver"] = metal((0.9, 0.9, 0.92), 0.8)
SHADERS["Metal_Brass"] = metal((0.85, 0.7, 0.4), 0.6)
SHADERS["Metal_BrushedSteel"] = metal((0.6, 0.62, 0.65), 0.4)

SHADERS["Plastic_Glossy"] = plastic((0.8, 0.1, 0.1), 0.0, 0.9)
SHADERS["Plastic_Matte"] = plastic((0.2, 0.5, 0.3), 0.0, 0.2)
SHADERS["Rubber_Black"] = plastic((0.03, 0.03, 0.03), 0.0, 0.1)
SHADERS["Porcelain_White"] = plastic((0.95, 0.95, 0.93), 0.0, 0.95)

SHADERS["Glass_Clear"] = glass((0.9, 0.95, 1.0), 0.25)
SHADERS["Glass_Tinted"] = glass((0.3, 0.8, 0.5), 0.4)
SHADERS["Ice_Translucent"] = glass((0.7, 0.9, 1.0), 0.55)

SHADERS["ForceField_Blue"] = force_field((0.3, 0.7, 1.0))
SHADERS["ForceField_Red"] = force_field((1.0, 0.3, 0.3))

SHADERS["XRay_Ghost"] = xray((0.8, 0.9, 1.0))
SHADERS["FlatColor_Unlit"] = flat((1.0, 1.0, 1.0))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, build in SHADERS.items():
        g = build()
        with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(g.to_json(), f, indent=1)
    print(f"wrote {len(SHADERS)} graphs to {OUT}")


if __name__ == "__main__":
    main()
