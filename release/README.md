# release/ — the `doom` publisher key

`publisher.seed` is the engine repo's Ed25519 release-signing seed
(64 lowercase hex, RFC 8032). Per VirelaiOS ADR 0058 D6 the `doom`
release stream is signed under a publisher key distinct from the
VirelaiOS base key — "software from anyone" means the payload carries
its own identity. Consumers pin the derived public key in
`FLINT/TRUSTED.TXT` as `name=doom`; the public half is printed to
`build/release/publisher.pub` by `make release` (`tools/release.py`).

The seed is committed deliberately: this is the dogfood publisher
identity for the M130 store drill, not a production secret. Trust never
comes from the seed staying private — it comes from each VM's keyring
choosing to pin `name=doom`. Anyone holding this file can mint `doom`
releases; only installs on keyrings that pin this key accept them.
