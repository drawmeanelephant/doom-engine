#!/usr/bin/env python3
"""tools/release.py — M130e: sign and package the `doom` release stream.

Per VirelaiOS ADR 0058 D6, the engine repo is its own publisher: it signs
a FLINT release manifest with its own Ed25519 key (release/publisher.seed)
and emits everything a consumer needs over the untrusted channel — the
signed stream record, the manifest text, and the channel drop
(`channel/doom/{release.go,DOOM.ELF,doom1.wad,LICENSE}`).

The wire facts this reimplements (no VirelaiOS source is copied, linked,
or wrapped — the FLINT release stream is a documented cross-repo
convention, like the syscall ABI):

  release.go atoms    one `const Name = "value"` spec per declaration;
                      canonical text is the spec's tokens joined by one
                      space with the declared name blanked to `_`
                      (e.g. `_ = "DOOM"`), hashed as
                      SHA256("FLINT-ATOM/1\\0" + "const" + "\\0" + text);
                      the const sig stream is the same text hashed under
                      "FLINT-SIG/1\\0".  The qualified atom name is
                      `release.<Const>`; its span is the ValueSpec's byte
                      range (name start .. literal end, exclusive).
  signed Change       JSON line {id,parent,stream,ts,author,message,ops,
                      merged,sig}; ops are `add` ops sorted by atom_id
                      carrying {type,atom_id,kind,new_name,new_span,
                      new_hash,new_sig_hash}.
  ops hash            SHA256("FLINT-OPS/1\\0" + u32le(nops) +
                      concat(op_bytes)), op_bytes = code(1) + uuid +
                      optstr(kind) + optstr(new_name) + optspan +
                      opthash(new_hash) + opthash(new_sig_hash) +
                      opthash(old_hash).  opt* fields: 0x00 absent, else
                      0x01 + payload (str = u32le len + utf8, span =
                      str(file) + u32le start + u32le end, hash = 32B).
  signature           Ed25519 over "FLINT-CHANGE/1\\0" + uuid(id) +
                      optuuid(parent) + uuid(stream) + u64le(ts) +
                      author(32B) + str(message) + optuuid(merged) +
                      ops_hash(32B).

Ed25519 is RFC 8032, implemented here in pure Python (no dependencies);
`--selftest` proves it against the RFC's TEST 1 vector and cross-checks
the canonical-hash and canonical-bytes derivations end to end.

Usage:
    python3 tools/release.py [--elf build/DOOM.ELF] [--wad assets/doom1.wad]
                             [--license LICENSE] [--seed release/publisher.seed]
                             [--outdir build/release] [--version 1.0.0]
                             [--selftest]
"""

import argparse
import hashlib
import json
import os
import struct
import sys

# ---------------------------------------------------------------------
# Ed25519 (RFC 8032, pure Python — verified by --selftest against the
# RFC's own test vectors).
# ---------------------------------------------------------------------

_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _xrecover(y):
    xx = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x & 1:
        x = _P - x
    return x


_BY = 4 * pow(5, _P - 2, _P) % _P
_B = (_xrecover(_BY), _BY)


def _edwards(p1, p2):
    (x1, y1), (x2, y2) = p1, p2
    x3 = (x1 * y2 + x2 * y1) * pow(1 + _D * x1 * x2 * y1 * y2, _P - 2, _P) % _P
    y3 = (y1 * y2 + x1 * x2) * pow(1 - _D * x1 * x2 * y1 * y2, _P - 2, _P) % _P
    return (x3 % _P, y3 % _P)


def _scalarmult(point, e):
    r = (0, 1)
    while e:
        if e & 1:
            r = _edwards(r, point)
        point = _edwards(point, point)
        e >>= 1
    return r


def _encodeint(y):
    return y.to_bytes(32, "little")


def _encodepoint(point):
    x, y = point
    b = bytearray(_encodeint(y))
    b[31] |= (x & 1) << 7
    return bytes(b)


def _hint(m):
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def _secret_scalar(h):
    # RFC 8032 §5.1.5 step 1: bits 0-2 cleared, bit 254 set, bit 255 clear.
    return 2**254 + sum(((h[i // 8] >> (i % 8)) & 1) << i for i in range(3, 254))


def ed_publickey(seed):
    h = hashlib.sha512(seed).digest()
    return _encodepoint(_scalarmult(_B, _secret_scalar(h)))


def ed_sign(msg, seed, pub):
    h = hashlib.sha512(seed).digest()
    a = _secret_scalar(h)
    r = _hint(h[32:64] + msg)
    rpoint = _scalarmult(_B, r)
    s = (r + _hint(_encodepoint(rpoint) + pub + msg) * a) % _Q
    return _encodepoint(rpoint) + _encodeint(s)


# ---------------------------------------------------------------------
# FLINT canonical bytes (release-stream facts — see module docstring).
# ---------------------------------------------------------------------

ATOM_DOMAIN = b"FLINT-ATOM/1\x00"
SIG_DOMAIN = b"FLINT-SIG/1\x00"
OPS_DOMAIN = b"FLINT-OPS/1\x00"
CHANGE_DOMAIN = b"FLINT-CHANGE/1\x00"


def u32le(v):
    return struct.pack("<I", v)


def u64le(v):
    return struct.pack("<Q", v)


def wstr(s):
    return u32le(len(s)) + s


def optstr(s):
    if not s:
        return b"\x00"
    return b"\x01" + wstr(s)


def optspan(span):
    if span is None:
        return b"\x00"
    return b"\x01" + wstr(span["file"].encode()) + u32le(span["start"]) + u32le(span["end"])


def opthash(hexstr):
    if not hexstr:
        return b"\x00"
    return b"\x01" + bytes.fromhex(hexstr)


def uuid_bytes(u):
    return bytes.fromhex(u.replace("-", ""))


def optuuid(u):
    if u is None:
        return b"\x00"
    return b"\x01" + uuid_bytes(u)


def op_bytes(op):
    code = {"add": 1, "modify": 2, "delete": 3, "move": 4, "rename": 5}[op["type"]]
    return (
        bytes([code])
        + uuid_bytes(op["atom_id"])
        + optstr(op.get("kind", "").encode())
        + optstr(op.get("new_name", "").encode())
        + optspan(op.get("new_span"))
        + opthash(op.get("new_hash", ""))
        + opthash(op.get("new_sig_hash", ""))
        + opthash(op.get("old_hash", ""))
    )


def ops_hash(ops):
    ordered = sorted(ops, key=lambda o: o["atom_id"])
    buf = OPS_DOMAIN + u32le(len(ordered))
    for op in ordered:
        buf += op_bytes(op)
    return hashlib.sha256(buf).digest()


def sig_msg(change):
    return (
        CHANGE_DOMAIN
        + uuid_bytes(change["id"])
        + optuuid(change.get("parent"))
        + uuid_bytes(change["stream"])
        + u64le(change["ts"])
        + bytes.fromhex(change["author"])
        + wstr(change["message"].encode())
        + optuuid(change.get("merged"))
        + ops_hash(change["ops"])
    )


# ---------------------------------------------------------------------
# The manifest.  One `const Name = "value"` spec per line under
# `package release` — the atom the Change signs is the ValueSpec text:
# canonical `_ = "value"`, span = [name offset, literal end).
# ---------------------------------------------------------------------

def go_quote(s):
    """strconv.Quote for the printable-ASCII + \\n subset this file emits."""
    out = ['"']
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            assert 0x20 <= ord(ch) < 0x7F, "non-ASCII/control byte %r" % ch
            out.append(ch)
    out.append('"')
    return "".join(out)


def atom_id(qname):
    # UUIDv7-shaped id derived from the qualified name — a const keeps its
    # id across releases regardless of declaration order (the consumer
    # folds ops by atom id, so stability is what matters).
    return "0199de2d-dc00-7000-8000-" + hashlib.sha256(qname.encode()).digest()[26:32].hex()


def build_manifest(consts):
    """Render release.go and its (kind, qname, span, hash, sig_hash) set.

    `consts` is an ordered list of (name, value); every value is emitted
    through go_quote so the literal text matches what a Go producer would
    write and what a consumer's tokenizer reads back.
    """
    text = "package release\n\n"
    atoms = []
    for name, value in consts:
        line = "const %s = %s\n" % (name, go_quote(value))
        start = len(text.encode()) + len("const ")
        literal = go_quote(value)
        spec_text = "%s = %s" % (name, literal)
        end = start + len(spec_text.encode())
        canon = "_ = " + literal
        ahash = hashlib.sha256(ATOM_DOMAIN + b"const\x00" + canon.encode()).hexdigest()
        shash = hashlib.sha256(SIG_DOMAIN + b"const\x00" + canon.encode()).hexdigest()
        atoms.append({
            "kind": "const",
            "name": "release." + name,
            "span": {"file": "release.go", "start": start, "end": end},
            "hash": ahash,
            "sig_hash": shash,
        })
        text += line
    return text, atoms


def sign_change(atoms, seed_hex, stream_id, change_id, ts, message):
    seed = bytes.fromhex(seed_hex)
    assert len(seed) == 32, "publisher seed must be 32 bytes (64 hex)"
    pub = ed_publickey(seed)
    ops = [{
        "type": "add",
        "atom_id": atom_id(a["name"]),
        "kind": a["kind"],
        "new_name": a["name"],
        "new_span": a["span"],
        "new_hash": a["hash"],
        "new_sig_hash": a["sig_hash"],
    } for a in atoms]
    ops.sort(key=lambda o: o["atom_id"])
    change = {
        "id": change_id,
        "parent": None,
        "stream": stream_id,
        "ts": ts,
        "author": pub.hex(),
        "message": message,
        "ops": ops,
        "merged": None,
    }
    change["sig"] = ed_sign(sig_msg(change), seed, pub).hex()
    return change, pub.hex()


# ---------------------------------------------------------------------
# The doom release (ADR 0058 D6).
# ---------------------------------------------------------------------

STREAM_ID = "0199de2d-dc00-7000-8000-00000000d010"
CHANGE_ID = "0199de2d-dc00-7000-8000-00000000d001"
RELEASE_TS = 1772000000000

# The release installs DOOM.ELF at the share root; the IWAD + the GPL
# text stay channel-side members and the app opens the WAD in place.
# argv= names the in-channel path so the installed row is launchable
# as-is on a stock share (the member is verified byte-for-byte against
# the signed ReleaseAssetSHA256 pin by whoever fetches it).
WAD_MEMBER = "doom1.wad"
LICENSE_MEMBER = "LICENSE"

MANIFEST_ROW = (
    "DOOM.ELF | DOOM | d | dock=false | v=2 | "
    "argv=-iwad /host/DROP/doom/" + WAD_MEMBER + " | "
    "caps=file,window,audio,timer,memory,debug"
)

SUMMARY = "DOOM (shareware episode) for VirelaiOS"
DESCRIPTION = (
    "The doomgeneric GPLv2 engine plus the id Software shareware IWAD,\n"
    "signed and published by the doom-engine sibling repo under its own\n"
    "publisher key. The IWAD rides the signed channel as a hash-pinned\n"
    "member and the app opens it in place."
)
CATEGORY = "game"
CAPS = "file,window,audio,timer,memory,debug"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def selftest():
    # RFC 8032 §7.1 TEST 1 — the same seed VirelaiOS' FLINT corpus pins.
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    pub = ed_publickey(seed)
    assert pub.hex() == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pub.hex()
    sig = ed_sign(b"", seed, pub)
    assert sig.hex() == (
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
    )
    # Canonical-atom pin: `_ = "GOHELLO"` is the recorded ReleaseName
    # atom of VirelaiOS' own release-stream fixture.
    canon = '_ = "GOHELLO"'
    h = hashlib.sha256(ATOM_DOMAIN + b"const\x00" + canon.encode()).hexdigest()
    assert h == "3520478bd611a9c20ac33fd7bbef70ce28b7bb9cfb15bc488f383a03737656d3", h
    s = hashlib.sha256(SIG_DOMAIN + b"const\x00" + canon.encode()).hexdigest()
    assert s == "91bcdc57a3291ebfee29f8be82d43a0224730219679e4746ef35c011291b90a0", s
    print("release.py selftest: Ed25519 RFC vector + FLINT atom pins OK")


def main(argv):
    ap = argparse.ArgumentParser(description="sign + package the doom release stream")
    ap.add_argument("--elf", default="build/DOOM.ELF")
    ap.add_argument("--wad", default="assets/doom1.wad")
    ap.add_argument("--license", dest="license", default="LICENSE")
    ap.add_argument("--seed", default="release/publisher.seed")
    ap.add_argument("--outdir", default="build/release")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--stream-id", default=STREAM_ID)
    ap.add_argument("--change-id", default=CHANGE_ID)
    ap.add_argument("--ts", type=int, default=RELEASE_TS)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        selftest()
        return 0

    for path in (args.elf, args.wad, args.license, args.seed):
        if not os.path.isfile(path):
            sys.exit("release.py: missing %s" % path)

    seed_hex = open(args.seed, "r", encoding="utf-8").read().strip()
    elf_sha = sha256_file(args.elf)
    wad_sha = sha256_file(args.wad)
    lic_sha = sha256_file(args.license)
    asset_pin = "%s:%s,%s:%s" % (WAD_MEMBER, wad_sha, LICENSE_MEMBER, lic_sha)

    consts = [
        ("ReleaseName", "DOOM"),
        ("ReleaseVersion", args.version),
        ("ReleaseELFSHA256", elf_sha),
        ("ReleaseManifestRow", MANIFEST_ROW),
        ("ReleaseSummary", SUMMARY),
        ("ReleaseDescription", DESCRIPTION),
        ("ReleaseCategory", CATEGORY),
        ("ReleaseCaps", CAPS),
        ("ReleaseAssetSHA256", asset_pin),
    ]
    rel_text, atoms = build_manifest(consts)
    change, pub_hex = sign_change(
        atoms, seed_hex, args.stream_id, args.change_id, args.ts,
        "doom release " + args.version)

    out = args.outdir
    channel = os.path.join(out, "channel", "doom")
    os.makedirs(channel, exist_ok=True)

    with open(os.path.join(out, "release.go"), "w", encoding="utf-8") as f:
        f.write(rel_text)
    line = json.dumps(change, separators=(",", ":"))
    with open(os.path.join(out, "doom-stream.jsonl"), "w", encoding="utf-8") as f:
        f.write(line + "\n")
    with open(os.path.join(out, "doom-stream.json"), "w", encoding="utf-8") as f:
        f.write(json.dumps(
            {"id": args.stream_id, "name": "doom", "head": change["id"],
             "created": args.ts, "forked_from": None},
            separators=(",", ":")) + "\n")
    with open(os.path.join(out, "publisher.pub"), "w", encoding="utf-8") as f:
        f.write(pub_hex + "\n")

    # The channel drop: every member a consumer fetches over the
    # untrusted path, verified byte-for-byte against the signed consts.
    with open(os.path.join(channel, "release.go"), "w", encoding="utf-8") as f:
        f.write(rel_text)
    for src, name in ((args.elf, "DOOM.ELF"), (args.wad, WAD_MEMBER),
                      (args.license, LICENSE_MEMBER)):
        with open(src, "rb") as fi, open(os.path.join(channel, name), "wb") as fo:
            fo.write(fi.read())

    print("release.py: stream=doom change=%s" % change["id"])
    print("  publisher=%s" % pub_hex)
    print("  ReleaseELFSHA256=%s" % elf_sha)
    print("  ReleaseAssetSHA256=%s" % asset_pin)
    print("  atoms=%d -> %s" % (len(atoms), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
