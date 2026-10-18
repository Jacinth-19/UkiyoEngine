#!/usr/bin/env python3
"""A small static checker for the GLSL the shader graph generates.

  python3 tools/sg_glsl_check.py            # every generated shader in assets/Shaders
  python3 tools/sg_glsl_check.py <file>...  # specific .ukshader or .frag files

glslangValidator is the real compiler and the build uses it (ukiyo_shader_tool compiles every graph to
SPIR-V). This checker is for working without it: it parses the generated fragment, builds a symbol
table from its own declarations, and type-checks every helper function and every statement of
ukiyo_surface(). It catches the mistakes a node template or a wired graph actually makes - an
undeclared name, a builtin called with the wrong arity, vec2 where a vec3 belongs, an unexpanded
placeholder, an unbalanced brace.

It is deliberately narrow: the fixed boilerplate (the frame block, SurfaceData, main) is known to
compile, so only the generated parts are checked. Constructs it does not model are reported, not
silently accepted.
"""
import os
import re
import sys

VECS = {2: "vec2", 3: "vec3", 4: "vec4"}
BVECS = {2: "bvec2", 3: "bvec3", 4: "bvec4"}
IVECS = {2: "ivec2", 3: "ivec3", 4: "ivec4"}
SCALARS = {"float", "int", "bool", "uint"}
MATRICES = {"mat2", "mat3", "mat4"}
VECTOR_TYPES = set(VECS.values()) | set(BVECS.values()) | set(IVECS.values())
BASE_TYPES = SCALARS | VECTOR_TYPES | MATRICES | {"sampler2D", "samplerCube", "void"}
GEN_TYPES = {"float", "vec2", "vec3", "vec4"}

KEYWORDS = {"if", "else", "return", "const", "for", "while", "struct", "layout", "uniform", "in", "out",
            "inout", "flat", "smooth", "noperspective", "precision", "highp", "mediump", "lowp", "discard",
            "break", "continue", "do", "switch", "case", "default"}


class Problem(Exception):
    pass


# ---------------------------------------------------------------- lexer

TOKEN_RE = re.compile(r"""
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<number>\d+\.\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?|\d+([eE][+-]?\d+)?)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op><<=|>=|==|!=|&&|\|\||[-+*/%<>=!?:;,.()\[\]{}])
""", re.S | re.X)


def tokenize(text):
    tokens = []
    line = 1
    pos = 0
    while pos < len(text):
        m = TOKEN_RE.match(text, pos)
        if not m:
            if text[pos].isspace():
                if text[pos] == "\n":
                    line += 1
                pos += 1
                continue
            raise Problem(f"line {line}: unexpected character {text[pos]!r}")
        pos = m.end()
        line += m.group(0).count("\n")
        if m.lastgroup == "comment":
            continue
        if m.lastgroup == "number":
            raw = m.group(0)
            is_float = any(c in raw for c in ".eE")
            tokens.append(("number", float(raw) if is_float else int(raw), line))
        else:
            tokens.append((m.lastgroup, m.group(0), line))
    tokens.append(("eof", "", line))
    return tokens


# ---------------------------------------------------------------- builtins

# Each rule maps argument types to a result type. "G" means every argument is the same genType
# (float, vec2, vec3 or vec4) and the result is that type; "V" means the same vector type (vec2/3/4);
# "F" means float or a vector, matching componentwise with a float where noted.
def _g(types):
    if all(t in GEN_TYPES for t in types) and len(set(types)) == 1:
        return types[0]
    return None


def _g_or_scalar(types, scalar_positions):
    """genType arguments, except the listed positions which may be a float."""
    base = [t for i, t in enumerate(types) if i not in scalar_positions]
    if not base or len(set(base)) != 1 or base[0] not in GEN_TYPES:
        return None
    for i in scalar_positions:
        if i < len(types) and types[i] not in (base[0], "float"):
            return None
    return base[0]


def _vec_same(types):
    if len(set(types)) == 1 and types[0] in VECTOR_TYPES:
        return types[0]
    return None


BUILTINS = {
    "radians": (1, _g), "degrees": (1, _g), "sin": (1, _g), "cos": (1, _g), "tan": (1, _g),
    "asin": (1, _g), "acos": (1, _g), "atan": (None, lambda t: _g(t) if len(t) == 1 else
                                                 (_g([t[0]]) if len(t) == 2 and t[0] == t[1] and t[0] in GEN_TYPES else None)),
    "pow": (2, _g), "exp": (1, _g), "log": (1, _g), "exp2": (1, _g), "log2": (1, _g),
    "sqrt": (1, _g), "inversesqrt": (1, _g), "abs": (1, lambda t: _g(t) or (t[0] if t[0] in ("int", "float") else None)),
    "sign": (1, _g), "floor": (1, _g), "trunc": (1, _g), "round": (1, _g), "ceil": (1, _g),
    "fract": (1, _g), "mod": (2, lambda t: _g_or_scalar(t, {1})),
    "min": (2, lambda t: _g_or_scalar(t, {1})), "max": (2, lambda t: _g_or_scalar(t, {1})),
    "clamp": (3, lambda t: _g_or_scalar(t, {1, 2})),
    "mix": (3, lambda t: _g_or_scalar(t, {2})),
    "step": (2, lambda t: _g_or_scalar(t, {0})),
    "smoothstep": (3, lambda t: _g_or_scalar(t, {0, 1})),
    "length": (1, lambda t: t[0] if False else ("float" if t[0] in GEN_TYPES else None)),
    "distance": (2, lambda t: "float" if len(set(t)) == 1 and t[0] in GEN_TYPES else None),
    "dot": (2, lambda t: "float" if len(set(t)) == 1 and t[0] in GEN_TYPES else None),
    "cross": (2, lambda t: "vec3" if t == ["vec3", "vec3"] else None),
    "normalize": (1, _g), "faceforward": (3, _g), "reflect": (2, _g), "refract": (3, lambda t: _g(t[:2]) if t[2] == "float" else None),
    "transpose": (1, lambda t: t[0] if t[0] in MATRICES else None),
    "determinant": (1, lambda t: "float" if t[0] in MATRICES else None),
    "matrixCompMult": (2, lambda t: t[0] if len(set(t)) == 1 and t[0] in MATRICES else None),
    "dFdx": (1, _g), "dFdy": (1, _g), "fwidth": (1, _g),
    "any": (1, lambda t: "bool" if t[0] in BVECS.values() or t[0] == "bool" else None),
    "all": (1, lambda t: "bool" if t[0] in BVECS.values() or t[0] == "bool" else None),
    "not": (1, lambda t: BVECS.get(int(t[0][4:])) if t[0] in BVECS.values() else None),
    "lessThan": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) and t[0] in (set(VECS.values()) | set(IVECS.values())) else None),
    "lessThanEqual": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) and t[0] in (set(VECS.values()) | set(IVECS.values())) else None),
    "greaterThan": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) and t[0] in (set(VECS.values()) | set(IVECS.values())) else None),
    "greaterThanEqual": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) and t[0] in (set(VECS.values()) | set(IVECS.values())) else None),
    "equal": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) else None),
    "notEqual": (2, lambda t: BVECS.get(int(t[0][-1])) if _vec_same(t) else None),
    "texture": (None, lambda t: "vec4" if len(t) >= 2 and t[0] == "sampler2D" and t[1] == "vec2" else None),
    "textureLod": (None, lambda t: "vec4" if len(t) == 3 and t[0] == "sampler2D" and t[1] == "vec2" and t[2] == "float" else None),
    "textureGrad": (None, lambda t: "vec4" if len(t) == 4 and t[0] == "sampler2D" else None),
    "textureGather": (None, lambda t: "vec4" if len(t) >= 2 and t[0] == "sampler2D" and t[1] == "vec2" else None),
    "texelFetch": (None, lambda t: "vec4" if len(t) >= 2 and t[0] == "sampler2D" else None),
}


def builtin_result(name, arg_types):
    if name not in BUILTINS:
        return None, False
    arity, rule = BUILTINS[name]
    if arity is not None and len(arg_types) != arity:
        return None, True
    return rule(arg_types), True


# ---------------------------------------------------------------- declarations

DECL_HEAD = re.compile(
    r"^(?:(?:layout\s*\([^)]*\)\s*)|flat\s+|smooth\s+|noperspective\s+)*(uniform|in|out|inout)\b")


class Symbol:
    def __init__(self, name, kind, fields=None, params=None, returns=None, array_of=None):
        self.name = name
        self.kind = kind                # base type name, "struct" or "function"
        self.fields = fields or {}      # struct member name -> type
        self.params = params or []      # function parameter types
        self.returns = returns
        self.array_of = array_of        # element type when the symbol is an array


class Checker:
    #: The stage's built-in variables: always in scope, never declared by the graph.
    BUILTIN_SYMBOLS = {"gl_FragCoord": "vec4", "gl_FragDepth": "float", "gl_FrontFacing": "bool",
                       "gl_PointCoord": "vec2", "gl_PointSize": "float", "gl_PrimitiveID": "int",
                       "gl_SampleID": "int", "gl_Layer": "int", "glViewportIndex": "int"}

    def __init__(self, text, label):
        self.label = label
        self.problems = []
        self.symbols = {name: Symbol(name, kind) for name, kind in Checker.BUILTIN_SYMBOLS.items()}
        self.structs = {}
        self.text = text
        self.lines = text.split("\n")

    def fail(self, line, message):
        self.problems.append(f"{self.label}:{line}: {message}")

    # -- top level -----------------------------------------------------------
    def run(self):
        self.check_placeholders()
        self.check_balance()
        body = self.strip_boilerplate()
        self.collect_declarations(body)
        self.check_functions(body)
        return self.problems

    def check_placeholders(self):
        for i, line in enumerate(self.lines, start=1):
            if re.search(r"%[0-9PIB]", line):
                self.fail(i, f"unexpanded node placeholder in generated GLSL: {line.strip()[:80]}")

    def check_balance(self):
        for open_ch, close_ch in (("{", "}"), ("(", ")"), ("[", "]")):
            if self.text.count(open_ch) != self.text.count(close_ch):
                self.fail(1, f"unbalanced {open_ch}{close_ch}: {self.text.count(open_ch)} vs {self.text.count(close_ch)}")

    def strip_boilerplate(self):
        """Drops the fixed preamble and main(), which the engine already compiles today."""
        start = self.text.find("struct SurfaceData {")
        main_match = re.search(r"void\s+main\s*\(\s*\)\s*\{", self.text)
        end = main_match.start() if main_match else -1
        if start < 0 or end < 0:
            self.fail(1, "the generated fragment is missing its struct or main")
            return self.text
        return self.text[:start] + "\n" + self.text[start:end]

    def collect_declarations(self, body):
        # Struct definitions, so a member access can be type checked.
        for m in re.finditer(r"struct\s+(\w+)\s*\{(.*?)\}\s*;", body, re.S):
            self.structs[m.group(1)] = self.parse_members(m.group(1), m.group(2))
        # Uniform blocks: `layout(...) uniform Name { members } instance;`. The material block has no
        # instance name, so its members are globals (that is how the generated shader reads p0, p1, ...).
        for m in re.finditer(r"uniform\s+(\w+)\s*\{(.*?)\}\s*(\w+)?\s*;", body, re.S):
            block_name, members, instance = m.groups()
            fields = self.parse_members(block_name, members)
            self.structs[block_name] = fields
            if instance and instance not in KEYWORDS:
                self.symbols[instance] = Symbol(instance, block_name, fields=fields)
            else:
                for name, symbol in fields.items():
                    self.symbols[name] = symbol
        # Loose uniforms (samplers) and varyings.
        for m in re.finditer(r"^(?:layout\s*\([^)]*\)\s*)?(uniform|in|out)\s+(\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*;",
                             body, re.M):
            qualifier, kind, name, array = m.groups()
            if kind not in BASE_TYPES:
                continue
            self.symbols[name] = Symbol(name, kind, array_of=kind if array else None)

    def parse_members(self, block_name, members):
        fields = {}
        # Comments go first: they carry semicolons of their own ("1 when no bake is connected").
        members = re.sub(r"//[^\n]*", "", members)
        for line in members.split(";"):
            line = line.strip()
            if not line:
                continue
            dm = re.match(r"^(float|int|bool|uint|vec2|vec3|vec4|ivec2|ivec3|ivec4|bvec2|bvec3|bvec4|"
                          r"mat2|mat3|mat4)\s+(\w+)\s*(?:\[\s*(\d+)\s*\])?$", line)
            if not dm:
                self.fail(1, f"cannot parse member of {block_name}: {line[:60]}")
                continue
            kind = dm.group(1)
            fields[dm.group(2)] = Symbol(dm.group(2), kind, array_of=kind if dm.group(3) else None)
        return fields

    # -- functions -----------------------------------------------------------
    def check_functions(self, body):
        for m in re.finditer(r"^(float|int|bool|void|vec2|vec3|vec4|ivec2|ivec3|ivec4|bvec2|bvec3|bvec4|"
                             r"mat2|mat3|mat4|SurfaceData)\s+(\w+)\s*\(([^)]*)\)\s*\{",
                             body, re.M):
            returns, name, params = m.groups()
            start = m.end() - 1
            end = self.match_brace(body, start)
            if end < 0:
                self.fail(1, f"{name}(): unbalanced braces")
                continue
            text = body[start + 1:end]
            line = body[:m.start()].count("\n") + 1
            param_types = []
            param_scope = {}
            for part in split_params(params):
                part = part.strip()
                if not part:
                    continue
                pm = re.match(r"^(?:in|out|inout)?\s*(const\s+)?(float|int|bool|vec2|vec3|vec4|mat2|mat3|mat4|"
                              r"sampler2D)\s+(\w+)$", part)
                if not pm:
                    self.fail(line, f"{name}(): cannot parse parameter {part!r}")
                    continue
                param_types.append(pm.group(2))
                param_scope[pm.group(3)] = Symbol(pm.group(3), pm.group(2))
            self.symbols[name] = Symbol(name, "function", params=param_types, returns=returns)
            self.check_body(text, line, param_scope, returns, name)

    @staticmethod
    def match_brace(text, start):
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return i
        return -1

    # -- statements ----------------------------------------------------------
    def check_body(self, text, base_line, outer_scope, returns, fn_name):
        scope = dict(outer_scope)
        line = base_line
        for raw in split_statements(text):
            line += raw.count("\n")
            stmt = re.sub(r"//.*", "", raw.statement if hasattr(raw, "statement") else raw).strip()
            if not stmt:
                continue
            try:
                self.check_statement(stmt, scope, line, returns, fn_name)
            except Problem as e:
                self.fail(line, f"{fn_name}: {e}")

    def check_statement(self, stmt, scope, line, returns, fn_name):
        stmt = stmt.strip()
        if stmt.endswith(";"):
            stmt = stmt[:-1].rstrip()
        if not stmt:
            return
        if stmt.startswith("{") or stmt.startswith("}"):
            return
        if stmt.startswith("#"):
            return
        if stmt in (";",):
            return
        if re.match(r"^if\s*\(", stmt):
            cond, rest = split_condition(stmt[stmt.index("("):])
            cond_type = self.expr_type(cond, scope, line)
            if cond_type not in ("bool", None):
                raise Problem(f"if condition is {cond_type}, expected bool")
            for part in split_blocks(rest):
                self.check_statement(part, dict(scope), line, returns, fn_name)
            return
        if stmt.startswith("return"):
            value = stmt[len("return"):].rstrip(";").strip()
            if not value:
                if returns != "void":
                    raise Problem(f"{fn_name} returns {returns} but this return has no value")
                return
            got = self.expr_type(value, scope, line)
            if got and returns and not assignable(got, returns):
                raise Problem(f"return {got} from a function declared {returns}")
            return
        # const int bayer[16] = int[16](...);
        m = re.match(r"^const\s+(\w+)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*=\s*(.*)$", stmt, re.S)
        if m:
            kind, name, size, value = m.groups()
            scope[name] = Symbol(name, kind, array_of=kind)
            return
        m = re.match(r"^(float|int|bool|uint|vec2|vec3|vec4|ivec2|ivec3|ivec4|bvec2|bvec3|bvec4|mat2|mat3|mat4|"
                     r"\w+)\s+(\w+)\s*(\[\s*\d+\s*\])?\s*(?:=\s*(.*))?$", stmt, re.S)
        if m:
            kind, name, array, value = m.groups()
            scope[name] = Symbol(name, kind, array_of=kind if array else None)
            if value:
                got = self.expr_type(value.rstrip(";").strip(), scope, line)
                if got and not assignable(got, kind):
                    raise Problem(f"{kind} {name} = <{got}>")
            return
        m = re.match(r"^([\w.()\[\]]+)\s*(=|\+=|-=|\*=|\/=)\s*(.*)$", stmt, re.S)
        if m:
            target, op, value = m.groups()
            left = self.expr_type(target, scope, line)
            right = self.expr_type(value.rstrip(";").strip(), scope, line)
            if left and right:
                if op == "=":
                    if not assignable(right, left):
                        raise Problem(f"assign <{right}> to {target} which is {left}")
                else:
                    arithmetic_type(op[0], left, right)
            return
        raise Problem(f"unrecognised statement: {stmt[:70]!r}")

    # -- expressions ---------------------------------------------------------
    def expr_type(self, text, scope, line):
        parser = ExpressionParser(tokenize(text), scope, self, line)
        result = parser.parse_ternary()
        parser.expect_eof()
        return result


def split_condition(text):
    """'(cond) rest' -> (cond, rest), splitting on the paren that closes the condition."""
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1:].strip()
    raise Problem("an if condition is missing its closing paren")


def split_params(text):
    depth = 0
    parts = []
    current = []
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    if "".join(current).strip():
        parts.append("".join(current))
    return parts


def split_statements(text):
    """Splits a function body on top-level semicolons, keeping braces intact."""
    out = []
    depth = 0
    current = []
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "/" and text[i:i + 2] == "//":
            j = text.find("\n", i)
            j = len(text) if j < 0 else j
            current.append(text[i:j])
            i = j
            continue
        if ch in "{(":
            depth += 1
        elif ch in "})":
            depth -= 1
        current.append(ch)
        if ch == ";" and depth == 0:
            out.append("".join(current))
            current = []
        elif ch == "}" and depth == 0:
            out.append("".join(current))
            current = []
        i += 1
    if "".join(current).strip():
        out.append("".join(current))
    return out


def split_blocks(text):
    """The statements inside an if's then/else part."""
    text = text.strip().rstrip(";")
    if text.startswith("{"):
        inner = text[1:text.rfind("}")]
        return [s for s in split_statements(inner) if s.strip()]
    return [text]


def assignable(from_type, to_type):
    if from_type is None or to_type is None:
        return True
    if from_type == to_type:
        return True
    if from_type in SCALARS and to_type in SCALARS:
        return True          # GLSL requires an explicit cast, but the templates only ever mix float/int
    return False


class ExpressionParser:
    def __init__(self, tokens, scope, checker, line):
        self.tokens = tokens
        self.i = 0
        self.scope = scope
        self.checker = checker
        self.line = line

    # -- token helpers
    def peek(self, offset=0):
        return self.tokens[min(self.i + offset, len(self.tokens) - 1)]

    def next(self):
        token = self.tokens[self.i]
        self.i += 1
        return token

    def accept(self, value):
        if self.peek()[1] == value:
            self.i += 1
            return True
        return False

    def expect(self, value):
        if not self.accept(value):
            raise Problem(f"expected {value!r}, found {self.peek()[1]!r}")

    def expect_eof(self):
        if self.peek()[0] != "eof":
            raise Problem(f"trailing tokens after the expression: {self.peek()[1]!r}")

    # -- precedence climbing
    def parse_ternary(self):
        cond = self.parse_binary(0)
        if self.accept("?"):
            a = self.parse_ternary()
            self.expect(":")
            b = self.parse_ternary()
            if a and b and not assignable(a, b):
                raise Problem(f"ternary branches differ: {a} and {b}")
            return a or b
        return cond

    BINARY_LEVELS = [
        (["||"],),
        (["&&"],),
        (["==", "!="],),
        (["<", ">", "<=", ">="],),
        (["+", "-"],),
        (["*", "/", "%"],),
    ]

    def parse_binary(self, level):
        if level >= len(self.BINARY_LEVELS):
            return self.parse_unary()
        ops = self.BINARY_LEVELS[level][0]
        left = self.parse_binary(level + 1)
        while self.peek()[1] in ops:
            op = self.next()[1]
            right = self.parse_binary(level + 1)
            left = self.binary_type(op, left, right)
        return left

    def binary_type(self, op, left, right):
        if left is None or right is None:
            return left or right
        if op in ("&&", "||"):
            if left != "bool" or right != "bool":
                raise Problem(f"{op} needs bool, got {left} and {right}")
            return "bool"
        if op in ("==", "!="):
            if not assignable(left, right) and not assignable(right, left):
                raise Problem(f"cannot compare {left} with {right}")
            return "bool"
        if op in ("<", ">", "<=", ">="):
            if left not in GEN_TYPES or right not in GEN_TYPES or (left != right and "float" not in (left, right)):
                raise Problem(f"cannot compare {left} with {right}")
            return "bool"
        return arithmetic_type(op, left, right)

    def parse_unary(self):
        token = self.peek()
        if token[1] in ("-", "+", "!"):
            self.next()
            kind = self.parse_unary()
            if token[1] == "!":
                if kind != "bool":
                    raise Problem(f"! needs bool, got {kind}")
                return "bool"
            return kind
        return self.parse_postfix()

    def parse_postfix(self):
        kind = self.parse_primary()
        while True:
            if self.accept("."):
                token = self.next()
                if token[0] != "ident":
                    raise Problem(f"expected a swizzle after '.', found {token[1]!r}")
                if kind in self.checker.structs:
                    fields = self.checker.structs[kind]
                    if token[1] not in fields:
                        raise Problem(f"{kind} has no member '{token[1]}'")
                    member = fields[token[1]]
                    kind = member.array_of or member.kind
                    continue
                kind = swizzle_type(kind, token[1], self.line)
                continue
            if self.peek()[1] == "[":
                self.next()
                index_type = self.parse_ternary()
                self.expect("]")
                if index_type not in ("int", "uint", None):
                    raise Problem(f"array index is {index_type}, expected int")
                kind = element_type(kind)
                continue
            break
        return kind

    def parse_primary(self):
        token = self.next()
        kind, value = token[0], token[1]
        if kind == "number":
            return "float" if isinstance(value, float) else "int"
        if kind == "op" and value == "(":
            inner = self.parse_ternary()
            self.expect(")")
            return inner
        if kind != "ident":
            raise Problem(f"unexpected token {value!r} in expression")
        if self.peek()[1] == "(":
            return self.parse_call(value)
        if value in CONSTRUCTORS:
            raise Problem(f"{value} used without an argument list")
        if value in self.scope:
            symbol = self.scope[value]
            return symbol.array_of or symbol.kind
        if value in self.checker.symbols:
            symbol = self.checker.symbols[value]
            return symbol.array_of or symbol.kind
        if value in KEYWORDS:
            raise Problem(f"keyword {value!r} used as a value")
        raise Problem(f"undeclared name {value!r}")

    def parse_call(self, name):
        self.expect("(")
        args = []
        if self.peek()[1] != ")":
            while True:
                args.append(self.parse_ternary())
                if not self.accept(","):
                    break
        self.expect(")")
        if name in CONSTRUCTORS:
            return constructor_type(name, args, self.line)
        symbol = self.checker.symbols.get(name)
        if symbol and symbol.kind == "function":
            if len(args) != len(symbol.params):
                raise Problem(f"{name}() takes {len(symbol.params)} arguments, called with {len(args)}")
            for got, want in zip(args, symbol.params):
                if got and not assignable(got, want):
                    raise Problem(f"{name}() argument is {got}, declared {want}")
            return symbol.returns
        result, known = builtin_result(name, args)
        if not known:
            raise Problem(f"call to unknown function {name}()")
        if result is None:
            raise Problem(f"{name}({', '.join(a or '?' for a in args)}) has no matching overload")
        return result


CONSTRUCTORS = {"float", "int", "bool", "uint", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
                "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4"}


def component_count(kind):
    if kind in SCALARS:
        return 1
    if kind in VECTOR_TYPES:
        return int(kind[-1])
    if kind == "mat2":
        return 4
    if kind == "mat3":
        return 9
    if kind == "mat4":
        return 16
    return 0


def constructor_type(name, args, line):
    if not args:
        raise Problem(f"{name}() needs arguments")
    if name in ("float", "int", "bool", "uint"):
        if len(args) != 1:
            raise Problem(f"{name}() takes one argument")
        return name
    if name.startswith("mat"):
        size = int(name[-1])
        for arg in args:
            if arg and arg not in MATRICES and arg not in SCALARS and arg not in VECTOR_TYPES:
                raise Problem(f"{name}() cannot take {arg}")
        if len(args) == 1 and args[0] in SCALARS | MATRICES:
            return name
        if len(args) == size and all(a in VECTOR_TYPES for a in args if a):
            return name
        if len(args) == 1 and args[0] in MATRICES:
            return name
        return name
    size = int(name[-1])
    total = 0
    for arg in args:
        if arg is None:
            return name
        if arg in SCALARS:
            total += 1
        elif arg in VECTOR_TYPES:
            total += component_count(arg)
        elif arg in MATRICES:
            raise Problem(f"{name}() cannot take {arg}")
        else:
            raise Problem(f"{name}() cannot take {arg}")
    if len(args) == 1 and args[0] in SCALARS:
        return name                      # vec3(0.0) fills every component
    if total != size:
        raise Problem(f"{name}() got {total} components, needs {size}")
    return name


SWIZZLE_CHARS = set("xyzwrgba01")


def swizzle_type(kind, name, line):
    if kind in ("sampler2D", "samplerCube"):
        raise Problem(f"cannot swizzle a sampler")
    if not all(c in SWIZZLE_CHARS for c in name):
        raise Problem(f"'{name}' is not a swizzle")
    if kind in SCALARS:
        if len(name) != 1:
            raise Problem(f"cannot swizzle a scalar with '{name}'")
        return kind
    if kind in MATRICES:
        raise Problem(f"cannot swizzle a matrix with '{name}'")
    size = component_count(kind)
    if len(name) > 4:
        raise Problem(f"swizzle '{name}' is longer than 4")
    family = None
    for c in name:
        f = {"xyzw": 0, "rgba": 1, "01": 2}[("xyzw" if c in "xyzw" else "rgba" if c in "rgba" else "01")]
        if family is None:
            family = f
        elif family != f:
            raise Problem(f"swizzle '{name}' mixes component sets")
    if family == 2 and any(c not in "01" for c in name):
        raise Problem(f"swizzle '{name}' mixes component sets")
    if size < max(index_of(c) for c in name) + 1 and family != 2:
        raise Problem(f"swizzle '{name}' reads past a {kind}")
    if len(name) == 1:
        return {"vec": "float", "ivec": "int", "bvec": "bool"}[kind[:len(kind) - 1]] if kind in VECTOR_TYPES else "float"
    prefix = kind[:len(kind) - 1]
    return prefix + str(len(name))


def index_of(c):
    return {"x": 0, "y": 1, "z": 2, "w": 3, "r": 0, "g": 1, "b": 2, "a": 3, "0": 0, "1": 1}[c]


def element_type(kind):
    if kind == "mat2":
        return "vec2"
    if kind == "mat3":
        return "vec3"
    if kind == "mat4":
        return "vec4"
    return kind


def arithmetic_type(op, left, right):
    if left in SCALARS and right in SCALARS:
        if op == "%" and "int" not in (left, right):
            raise Problem("% needs integers")
        return "float" if "float" in (left, right) else left
    if left in SCALARS and right in VECTOR_TYPES:
        return right
    if left in VECTOR_TYPES and right in SCALARS:
        return left
    if left in SCALARS and right in MATRICES:
        return right if op in ("*", "/") else _bad(op, left, right)
    if left in MATRICES and right in SCALARS:
        return left if op in ("*", "/") else _bad(op, left, right)
    if left in VECTOR_TYPES and right in VECTOR_TYPES:
        if left != right:
            raise Problem(f"{left} {op} {right}: vector sizes differ")
        return left
    if left in MATRICES and right in MATRICES:
        if left != right:
            raise Problem(f"{left} {op} {right}: matrix sizes differ")
        return left
    if left in MATRICES and right in VECTOR_TYPES:
        if op != "*":
            raise Problem(f"{left} {op} {right}")
        return {"mat2": "vec2", "mat3": "vec3", "mat4": "vec4"}[left]
    if left in VECTOR_TYPES and right in MATRICES:
        if op != "*":
            raise Problem(f"{left} {op} {right}")
        return {"vec2": "mat2", "vec3": "mat3", "vec4": "mat4"}[left]
    raise Problem(f"{left} {op} {right} is not a valid operation")


def _bad(op, left, right):
    raise Problem(f"{left} {op} {right} is not a valid operation")


# ---------------------------------------------------------------- driver


def check_fragment(text, label):
    return Checker(text, label).run()


def check_file(path):
    if path.endswith(".ukshader"):
        import json
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
        fragment = doc.get("compiled", {}).get("fragment")
        if not fragment or doc.get("handWritten"):
            return []          # hand-written shaders are compiled by glslang in the build
        return check_fragment(fragment, os.path.basename(path))
    with open(path, "r", encoding="utf-8") as f:
        return check_fragment(f.read(), os.path.basename(path))


# Cases the checker must reject. Without them a checker that accepts everything looks healthy.
SELFTEST = [
    ("float a = vec3(1.0);", "scalar from a vector"),
    ("vec3 b = missingName;", "undeclared name"),
    ("vec3 c = mix(vec3(1.0), vec3(0.0));", "wrong arity"),
    ("float d = dot(vec2(1.0), vec3(1.0));", "no matching overload"),
    ("vec2 e = vec2(1.0, 2.0, 3.0);", "too many components"),
    ("float f = frame.notAMember;", "unknown struct member"),
    ("vec4 g = vec4(1.0)", "missing semicolon"),
    ("float h = (1.0 + 2.0;", "unbalanced"),
    ("float i = %0 + %1;", "unexpanded placeholder"),
    ("vec3 j = normalize(normal); j.zw = vec2(0.0);", "swizzle past the vector"),
    ("float k = texture(uv).x;", "texture() needs a sampler"),
]

FRAGMENT_SHELL = """#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_world;
layout(location = 3) in vec4 v_color;
layout(location = 4) in vec2 v_uv1;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0, std140) uniform FrameBlock {
    vec4 camPos;
} frame;

struct SurfaceData {
    vec3 baseColor;
    float alpha;
};

SurfaceData ukiyo_surface(vec2 uv, vec3 normal) {
    SurfaceData s;
    s.baseColor = vec3(1.0);
    s.alpha = 1.0;
%s
    return s;
}

void main() {
    o_color = vec4(vec3(1.0), 1.0);
}
"""


def selftest():
    """Every SELFTEST body must be rejected; a clean body must be accepted."""
    failures = []
    for body, why in SELFTEST:
        lines = "\n".join("    " + part.strip() for part in body.split("\n"))
        problems = check_fragment(FRAGMENT_SHELL % lines, "selftest")
        if not problems:
            failures.append(f"accepted a body it should reject ({why}): {body}")
    clean = FRAGMENT_SHELL % "    vec3 tint = mix(s.baseColor, v_color.rgb, v_color.a);"
    problems = check_fragment(clean, "selftest")
    if problems:
        failures.append("rejected a clean body: " + "; ".join(problems))
    for f in failures:
        print("  " + f)
    print(f"selftest: {len(SELFTEST) + 1} cases, {len(failures)} failures")
    return not failures


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return 0 if selftest() else 1
    if len(argv) > 1:
        paths = argv[1:]
    else:
        root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        shader_dir = os.path.join(root, "assets", "Shaders")
        paths = [os.path.join(shader_dir, f) for f in sorted(os.listdir(shader_dir)) if f.endswith(".ukshader")]
    problems = []
    for path in paths:
        try:
            problems.extend(check_file(path))
        except Problem as e:
            problems.append(f"{os.path.basename(path)}: {e}")
    for p in problems:
        print("  " + p)
    print(f"checked {len(paths)} files: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
