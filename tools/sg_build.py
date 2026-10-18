#!/usr/bin/env python3
"""Builds shader-graph JSON with the port-kind checks the editor makes.

QtNodes refuses a connection between ports of different types, and the code generator walks those
connections to emit GLSL. A generator script that gets a kind wrong would therefore write a graph the
editor silently repairs on load, and a .ukshader whose GLSL does not match its own graph. Every link
here goes through Builder.wire(), which checks both ends against tools/sg_mirror.py's node table - the
same table tools/sg_node_sync_test.py proves matches sg_nodes.cpp.

Values passed to the helpers may be a Ref (a node output) or a plain number/tuple, which becomes a
constant node (Float, Vector2 or Color). A Ref carries its kind, so `mul()` can pick MultiplyF,
MultiplyV3 or Scale (Vector 3 by Float) on its own.
"""
import sg_mirror

KIND_OF_PARAM = {"float": "float", "vec2": "vec2", "color": "vec3"}

#: A constant node per port kind, for graphs that just need every input driven.
CONSTANT = {"float": ("Float", ["0.5"]), "vec2": ("Vector2", ["0.25", "0.75"]),
            "vec3": ("Color", ["0.8", "0.4", "0.2"])}


class Ref:
    """One output port of a node in the graph."""

    __slots__ = ("builder", "id", "port", "kind", "spec")

    def __init__(self, builder, node_id, port):
        self.builder = builder
        self.id = node_id
        self.port = port
        spec = sg_mirror.find_spec(builder.model_of(node_id))
        self.spec = spec
        self.kind = spec.outputs[port].kind

    def __getitem__(self, port):
        """node[1] is the node's second output."""
        return Ref(self.builder, self.id, port)

    def __repr__(self):
        return f"<{self.spec.id}[{self.port}]:{self.kind}>"


class Builder:
    def __init__(self):
        self.nodes = []
        self.conns = []
        self.wired = set()          # (node id, input port) already connected

    # -- graph construction --------------------------------------------------
    def model_of(self, node_id):
        return self.nodes[node_id]["internal-data"]["model-name"]

    def node(self, model, params=(), reference=""):
        """Adds a node and returns a Ref to its first output (or a NodeHandle for masters)."""
        spec = sg_mirror.find_spec(model)
        if spec is None:
            raise KeyError(f"unknown node type {model!r}")
        params = [str(p) for p in params]
        expected = sg_mirror.PARAM_COUNT[spec.param]
        if spec.param == "texture":
            params = params or [""]
        elif len(params) < expected:
            params = params + sg_mirror.default_params(spec.param)[len(params):]
        if len(params) > expected and spec.param != "texture":
            raise ValueError(f"{model} takes {expected} parameter values, got {len(params)}")
        nid = len(self.nodes)
        self.nodes.append({
            "id": nid,
            "internal-data": {"model-name": model, "params": params, "reference": reference},
            "label": "",
            "labelVisible": True,
            "position": {"x": 0, "y": 0},
        })
        return Ref(self, nid, 0) if spec.outputs else Ref(self, nid, 0)

    def master(self, model, **inputs):
        """Adds a master node and wires the named inputs. Unnamed ports keep their defaults."""
        spec = sg_mirror.find_spec(model)
        if spec is None or not spec.master:
            raise KeyError(f"{model!r} is not a master node")
        by_name = {}
        for i, port in enumerate(spec.inputs):
            by_name[port.name.lower()] = i
        nid = len(self.nodes)
        self.nodes.append({
            "id": nid,
            "internal-data": {"model-name": model, "params": [], "reference": ""},
            "label": "",
            "labelVisible": True,
            "position": {"x": 0, "y": 0},
        })
        for name, value in inputs.items():
            key = name.lower().replace("_", " ")
            if key not in by_name:
                raise KeyError(f"{model} has no input {name!r} (it has {sorted(by_name)})")
            if value is None:
                continue
            self.wire(value, nid, by_name[key])
        return nid

    def wire(self, value, node_id, port):
        """Connects `value` (a Ref or a constant) to input `port` of `node_id`, kind checked."""
        spec = sg_mirror.find_spec(self.model_of(node_id))
        if port >= len(spec.inputs):
            raise IndexError(f"{spec.id} has no input port {port}")
        want = spec.inputs[port].kind
        ref = self.ref(value, want)
        if ref.kind != want:
            raise TypeError(f"{spec.id}.{spec.inputs[port].name} needs {want}, got {ref.kind} "
                            f"from {ref.spec.id}.{ref.spec.outputs[ref.port].name}")
        key = (node_id, port)
        if key in self.wired:
            raise ValueError(f"{spec.id} input {port} ({spec.inputs[port].name}) is wired twice")
        self.wired.add(key)
        self.conns.append({"inNodeId": node_id, "inPortIndex": port,
                           "outNodeId": ref.id, "outPortIndex": ref.port})
        return ref

    def fill_inputs(self, node_id):
        """Wires a constant of the right kind to every input the graph has not connected yet. Coverage
        graphs use this to reach every node's code; a style never needs it."""
        spec = sg_mirror.find_spec(self.model_of(node_id))
        for i, port in enumerate(spec.inputs):
            if (node_id, i) in self.wired:
                continue
            self.wire(self.node(*CONSTANT[port.kind]), node_id, i)
        return node_id

    def ref(self, value, kind):
        """A Ref for `value`: Refs pass through, numbers and tuples become constant nodes."""
        if isinstance(value, Ref):
            return value
        if kind == "float":
            return self.node("Float", [value])
        if kind == "vec2":
            x, y = value if isinstance(value, (tuple, list)) else (value, value)
            return self.node("Vector2", [x, y])
        if kind == "vec3":
            rgb = value if isinstance(value, (tuple, list)) else (value, value, value)
            return self.node("Color", list(rgb))
        raise TypeError(f"no constant node for kind {kind}")

    def graph(self):
        """The graph JSON, laid out left to right in the order the nodes were added."""
        for i, node in enumerate(self.nodes):
            node["position"] = {"x": (i % 7) * 240, "y": (i // 7) * 140}
        return {"connections": self.conns, "nodes": self.nodes}

    # -- values --------------------------------------------------------------
    def color(self, rgb, reference=""):
        return self.node("Color", list(rgb), reference)

    def float_(self, value, reference=""):
        return self.node("Float", [value], reference)

    def vec2(self, xy, reference=""):
        return self.node("Vector2", list(xy), reference)

    def texture(self, path="", reference="", uv=None):
        node = self.node("SampleTexture2D", [path], reference)
        if uv is not None:
            self.wire(uv, node.id, 0)
        return node

    # -- float maths ---------------------------------------------------------
    def add(self, a, b):
        a, b = self.promote(a, b)
        return self.binary("AddF", "AddV3", a, b)

    def sub(self, a, b):
        a, b = self.promote(a, b)
        return self.binary("SubtractF", "SubtractV3", a, b)

    def mul(self, a, b):
        ra, rb = self.ref_of(a), self.ref_of(b)
        if ra.kind == "float" and rb.kind == "float":
            return self.binary("MultiplyF", "MultiplyV3", ra, rb)
        if ra.kind == "vec3" and rb.kind == "vec3":
            return self.binary("MultiplyF", "MultiplyV3", ra, rb)
        if ra.kind == "vec3" and rb.kind == "float":
            return self.scale(ra, rb)
        if ra.kind == "float" and rb.kind == "vec3":
            return self.scale(rb, ra)
        raise TypeError(f"cannot multiply {ra.kind} by {rb.kind}")

    def binary(self, float_model, vec_model, a, b):
        kind = a.kind
        model = float_model if kind == "float" else vec_model
        if model is None:
            raise TypeError(f"no {kind} node for this operation")
        node = self.node(model)
        self.wire(a, node.id, 0)
        self.wire(b, node.id, 1)
        return node

    def promote(self, a, b):
        """A float and a vec3 become two vec3s, so Add and Subtract stay componentwise."""
        ra, rb = self.ref_of(a), self.ref_of(b)
        if ra.kind == rb.kind:
            return ra, rb
        if ra.kind == "float" and rb.kind == "vec3":
            return self.splat(ra), rb
        if ra.kind == "vec3" and rb.kind == "float":
            return ra, self.splat(rb)
        raise TypeError(f"cannot combine {ra.kind} with {rb.kind}")

    def ref_of(self, value):
        """A Ref for a value in an arithmetic helper. Only Refs and bare numbers are accepted: a tuple
        is ambiguous (vec2 or vec3?), so build the constant node yourself."""
        if isinstance(value, Ref):
            return value
        if isinstance(value, (int, float)):
            return self.node("Float", [value])   # a bare number in an expression is a float
        raise TypeError(f"cannot use {value!r} as a value: use color()/vec2() to build the constant")

    def scale(self, vector, factor):
        node = self.node("ScaleV3")
        self.wire(vector, node.id, 0)
        self.wire(factor, node.id, 1)
        return node

    def splat(self, value):
        """A float as a vec3 (Combine RGB with the same value on all three)."""
        node = self.node("Combine")
        for port in range(3):
            self.wire(value, node.id, port)
        return node

    def lerp(self, a, b, t):
        ra, rb = self.promote(a, b)
        model = "LerpF" if ra.kind == "float" else "LerpV3"
        node = self.node(model)
        self.wire(ra, node.id, 0)
        self.wire(rb, node.id, 1)
        self.wire(t, node.id, 2)
        return node

    def one_minus(self, a):
        node = self.node("OneMinusF")
        self.wire(a, node.id, 0)
        return node

    def saturate(self, a):
        ref = self.ref_of(a)
        model = "SaturateF" if ref.kind == "float" else "SaturateV3"
        node = self.node(model)
        self.wire(ref, node.id, 0)
        return node

    def smoothstep(self, edge0, edge1, x):
        node = self.node("SmoothstepF")
        self.wire(edge0, node.id, 0)
        self.wire(edge1, node.id, 1)
        self.wire(x, node.id, 2)
        return node

    def clamp(self, x, low, high):
        node = self.node("ClampF")
        self.wire(low, node.id, 0)
        self.wire(high, node.id, 1)
        self.wire(x, node.id, 2)
        return node

    def remap(self, x, in_min, in_max, out_min, out_max):
        node = self.node("Remap")
        for port, value in enumerate((x, in_min, in_max, out_min, out_max)):
            self.wire(value, node.id, port)
        return node

    def power(self, base, exponent):
        node = self.node("PowerF")
        self.wire(base, node.id, 0)
        self.wire(exponent, node.id, 1)
        return node

    def fract(self, a):
        node = self.node("FractF")
        self.wire(a, node.id, 0)
        return node

    def sine(self, a):
        node = self.node("SineF")
        self.wire(a, node.id, 0)
        return node

    def posterize(self, a, steps):
        node = self.node("Posterize")
        self.wire(a, node.id, 0)
        self.wire(steps, node.id, 1)
        return node

    # -- vector maths --------------------------------------------------------
    def dot(self, a, b):
        node = self.node("DotV3")
        self.wire(a, node.id, 0)
        self.wire(b, node.id, 1)
        return node

    def normalize(self, a):
        node = self.node("NormalizeV3")
        self.wire(a, node.id, 0)
        return node

    def luminance(self, a):
        node = self.node("Luminance")
        self.wire(a, node.id, 0)
        return node

    def desaturate(self, a, saturation):
        node = self.node("Desaturate")
        self.wire(a, node.id, 0)
        self.wire(saturation, node.id, 1)
        return node

    def hue_rotate(self, a, degrees):
        node = self.node("HueRotate")
        self.wire(a, node.id, 0)
        self.wire(degrees, node.id, 1)
        return node

    def combine2(self, x, y):
        node = self.node("Combine2")
        self.wire(x, node.id, 0)
        self.wire(y, node.id, 1)
        return node

    def split2(self, value):
        node = self.node("Split2")
        self.wire(value, node.id, 0)
        return node            # node[0] = X, node[1] = Y

    def split3(self, value):
        node = self.node("Split")
        self.wire(value, node.id, 0)
        return node            # node[0] = R, node[1] = G, node[2] = B

    def uv_transform(self, uv, tiling, offset):
        node = self.node("TilingAndOffset")
        self.wire(uv, node.id, 0)
        self.wire(tiling, node.id, 1)
        self.wire(offset, node.id, 2)
        return node

    # -- the anime building blocks -------------------------------------------
    def normal(self):
        return self.node("Normal")

    def sun(self):
        return self.node("SunDirection")

    def view(self):
        return self.node("ViewDirection")

    def sun_color(self):
        return self.node("SunColor")

    def ambient(self):
        return self.node("AmbientColor")

    def time(self):
        return self.node("Time")

    def uv(self):
        return self.node("UV")

    def world(self):
        return self.node("WorldPosition")

    def vertex_color(self):
        return self.node("VertexColor")

    def screen_position(self):
        return self.node("ScreenPosition")

    def half_lambert(self, normal=None, wrap=0.0):
        """The sun term every cel style starts from: N.L remapped so the terminator sits where `wrap`
        moves it. wrap 0 splits at 90 degrees, 0.5 pushes light further around the form."""
        n = self.ref_of(normal) if normal is not None else self.normal()
        ndl = self.dot(n, self.sun())
        return self.remap(ndl, -1.0 + wrap, 1.0, 0.0, 1.0)

    def cel(self, light, bands=2.0, softness=0.1, bands_ref="", softness_ref=""):
        """CelBands with the two controls exposed as material properties when names are given."""
        node = self.node("CelBands")
        self.wire(light, node.id, 0)
        self.wire(self.float_(bands, bands_ref) if bands_ref else bands, node.id, 1)
        self.wire(self.float_(softness, softness_ref) if softness_ref else softness, node.id, 2)
        return node

    def fresnel(self, power=4.0, reference="", normal=None):
        node = self.node("Fresnel")
        if normal is not None:
            self.wire(normal, node.id, 0)
        self.wire(self.float_(power, reference) if reference else power, node.id, 1)
        return node

    def scene_light(self, ramp, ambient=0.55, floor_=0.0, ambient_ref=""):
        """Sun colour scaled by the ramp plus hemisphere ambient: the engine's toon branch as a graph,
        so an Unlit Master style still answers to the scene's sun and sky."""
        sun = self.scale(self.sun_color(), self.add(ramp, floor_) if floor_ else ramp)
        strength = self.float_(ambient, ambient_ref) if ambient_ref else ambient
        return self.add(sun, self.scale(self.ambient(), strength))

    def light_flat(self, sun=1.0, ambient=0.55, sun_ref="", ambient_ref=""):
        """Scene light with no ramp in it, for styles whose tones are already decided: sun colour times
        a strength, plus hemisphere ambient. Keeps an Unlit Master style answering to the scene."""
        sun_term = self.scale(self.sun_color(), self.float_(sun, sun_ref) if sun_ref else sun)
        amb_term = self.scale(self.ambient(), self.float_(ambient, ambient_ref) if ambient_ref else ambient)
        return self.add(sun_term, amb_term)

    def cel_lit(self, albedo, ramp, ambient=0.55, floor_=0.0):
        """The usual finish for an Unlit Master style: banded albedo under the scene's light."""
        return self.mul(albedo, self.scene_light(ramp, ambient, floor_))

    def abs_(self, a):
        node = self.node("AbsF")
        self.wire(a, node.id, 0)
        return node

    def gradient(self, low, mid, high, t):
        """Gradient3: `low` at t=0, `mid` at 0.5, `high` at 1. A banded t gives flat tone steps."""
        node = self.node("Gradient3")
        for port, value in enumerate((low, mid, high, t)):
            self.wire(value, node.id, port)
        return node

    def noise(self, model, uv=None, scale=8.0, scale_ref=""):
        node = self.node(model)
        if uv is not None:
            self.wire(uv, node.id, 0)
        self.wire(self.float_(scale, scale_ref) if scale_ref else scale, node.id, 1)
        return node

    def halftone(self, value, uv=None, frequency=24.0, angle=45.0, frequency_ref="", angle_ref=""):
        """Halftone dots: Out is 1 where the ink covers, so feed it 1-ramp for shadows."""
        node = self.node("Halftone")
        if uv is not None:
            self.wire(uv, node.id, 0)
        self.wire(self.float_(frequency, frequency_ref) if frequency_ref else frequency, node.id, 1)
        self.wire(self.float_(angle, angle_ref) if angle_ref else angle, node.id, 2)
        self.wire(value, node.id, 3)
        return node

    def hatch(self, value, uv=None, frequency=40.0, angle=45.0, frequency_ref="", angle_ref=""):
        """Hatching: Out is 1 on the strokes, so feed it 1-ramp for shadows."""
        node = self.node("HatchLines")
        if uv is not None:
            self.wire(uv, node.id, 0)
        self.wire(self.float_(frequency, frequency_ref) if frequency_ref else frequency, node.id, 1)
        self.wire(self.float_(angle, angle_ref) if angle_ref else angle, node.id, 2)
        self.wire(value, node.id, 3)
        return node

    def scanlines(self, line_height=3.0, speed=0.0, height_ref="", speed_ref=""):
        node = self.node("Scanlines")
        self.wire(self.float_(line_height, height_ref) if height_ref else line_height, node.id, 0)
        self.wire(self.float_(speed, speed_ref) if speed_ref else speed, node.id, 1)
        return node

    def bayer(self, pixel_size=1.0, reference=""):
        node = self.node("BayerDither")
        self.wire(self.float_(pixel_size, reference) if reference else pixel_size, node.id, 0)
        return node

    def pixelate(self, uv, blocks=64.0, reference=""):
        node = self.node("PixelateUV")
        self.wire(uv, node.id, 0)
        self.wire(self.float_(blocks, reference) if reference else blocks, node.id, 1)
        return node

    def spec_band(self, normal=None, threshold=0.9, softness=0.02, threshold_ref="", softness_ref=""):
        node = self.node("SpecBand")
        if normal is not None:
            self.wire(normal, node.id, 0)
        self.wire(self.float_(threshold, threshold_ref) if threshold_ref else threshold, node.id, 1)
        self.wire(self.float_(softness, softness_ref) if softness_ref else softness, node.id, 2)
        return node

    def hair_spec(self, normal=None, shift=0.3, exponent=40.0, strength=1.0,
                  shift_ref="", exponent_ref="", strength_ref=""):
        """The two-lobe anisotropic ring: one node already returns both lobes summed."""
        node = self.node("HairSpecular")
        if normal is not None:
            self.wire(normal, node.id, 0)
        for port, (value, ref) in enumerate(((shift, shift_ref), (exponent, exponent_ref),
                                             (strength, strength_ref)), start=1):
            self.wire(self.float_(value, ref) if ref else value, node.id, port)
        return node

    def matcap(self, reference="matcapMap", normal=None, path=""):
        """A matcap sample: the cheap spherical sheen anime hair, eyes and metal lean on."""
        uv = self.node("MatcapUV")
        if normal is not None:
            self.wire(normal, uv.id, 0)
        return self.texture(path, reference, uv=uv)

    def face_angle(self):
        """Face Light Angle: Out[0] is left/right, Out[1] front/back, both -1..1."""
        return self.node("FaceLightAngle")

    def face_sdf(self, sdf, bias=0.0, softness=0.05, bias_ref="", softness_ref=""):
        node = self.node("FaceSDFTerm")
        self.wire(sdf, node.id, 0)
        self.wire(self.float_(bias, bias_ref) if bias_ref else bias, node.id, 1)
        self.wire(self.float_(softness, softness_ref) if softness_ref else softness, node.id, 2)
        return node

    def albedo(self, tint=(1.0, 1.0, 1.0), tint_ref="baseColor", map_ref="baseMap"):
        """Texture times tint: the usual way an anime material takes its colour, so a recolour or a
        plain-colour prop needs no map at all (an unassigned slot samples white)."""
        return self.mul(self.texture("", map_ref), self.color(tint, tint_ref))

    def rim(self, normal=None, color=(1.0, 1.0, 1.0), color_ref="rimColor", power=4.0,
            power_ref="rimPower", strength=1.0, strength_ref="rimStrength"):
        """Rim light as an additive emission term: fresnel mask, colour, strength."""
        mask = self.fresnel(power, power_ref, normal)
        if strength_ref or strength != 1.0:
            mask = self.mul(mask, self.float_(strength, strength_ref) if strength_ref else strength)
        return self.scale(self.color(color, color_ref), mask)

    def specular(self, normal=None, color=(1.0, 1.0, 1.0), color_ref="specColor", threshold=0.9,
                 threshold_ref="specThreshold", softness=0.02, softness_ref="specSoftness",
                 strength=1.0, strength_ref="specStrength"):
        """The hard anime specular shape as an additive emission term."""
        band = self.spec_band(normal, threshold, softness, threshold_ref, softness_ref)
        if strength_ref or strength != 1.0:
            band = self.mul(band, self.float_(strength, strength_ref) if strength_ref else strength)
        return self.scale(self.color(color, color_ref), band)

    def shade_with(self, albedo, ramp, shadow_color, shadow_ref="shadowColor"):
        """Two-tone tinting the way Toon Master does it: the shadow colour multiplies the albedo."""
        return self.mul(albedo, self.lerp(self.color(shadow_color, shadow_ref), self.color((1.0, 1.0, 1.0)), ramp))

    def glow(self, *terms):
        """Sums additive terms: into Emission on a lit master, into Color on Unlit. None when empty, so
        a master input can keep its default."""
        terms = [t for t in terms if t is not None]
        if not terms:
            return None
        out = terms[0]
        for term in terms[1:]:
            out = self.add(out, term)
        return out

    def masked(self, color, mask, strength):
        """Scales a colour by a 0..1 mask at `strength`: mask 1 keeps the colour, mask 0 darkens it by
        the strength. The one-liner behind scanlines, grain and vignettes."""
        factor = self.add(self.mul(mask, strength), self.sub(1.0, strength))
        return self.mul(color, self.splat(factor))

    def uv_scroll(self, uv=None, speed=(0.0, 0.0), tiling=(1.0, 1.0)):
        """UV scrolled by Time times `speed`: how noise, water and light sweeps animate."""
        t = self.time()
        offset = self.combine2(self.mul(t, speed[0]), self.mul(t, speed[1]))
        return self.uv_transform(self.uv() if uv is None else uv, tiling, offset)

    def outline(self, color=(0.12, 0.09, 0.13), width=0.02, alpha=1.0):
        """The three exposed values that ask the engine for the inverted-hull outline pass. They do not
        feed the master: shaders/graph_outline.vert reads them straight from the material block."""
        self.color(color, "outlineColor")
        self.float_(width, "outlineWidth")
        self.float_(alpha, "outlineAlpha")
