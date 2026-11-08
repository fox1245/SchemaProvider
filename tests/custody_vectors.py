#!/usr/bin/env python3
"""Independent HKDF-SHA256 reference vectors for derive_kek using only hmac and hashlib."""

import hashlib
import hmac


def encode_text(value: bytes) -> bytes:
    return len(value).to_bytes(8, "big") + value


def hkdf_sha256_extract(salt: bytes, ikm: bytes) -> bytes:
    if not salt:
        salt = b"\x00" * hashlib.sha256().digest_size
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_sha256_expand(prk: bytes, info: bytes, length: int) -> bytes:
    hash_len = hashlib.sha256().digest_size
    blocks = (length + hash_len - 1) // hash_len
    okm = bytearray()
    prev = b""
    for counter in range(1, blocks + 1):
        prev = hmac.new(prk, prev + info + bytes([counter]), hashlib.sha256).digest()
        okm.extend(prev)
    return bytes(okm[:length])


def derive_kek(secret: bytes, store_id: bytes) -> bytes:
    assert 32 <= len(secret) <= 64
    assert len(store_id) == 32
    kek_domain = b"sp.native.archive.custody.kek.v1"
    prk = hkdf_sha256_extract(store_id, secret)
    info = encode_text(kek_domain)
    return hkdf_sha256_expand(prk, info, 32)


def main() -> None:
    store_id = bytes(range(0x00, 0x20))
    secret_32 = bytes(range(0x20, 0x40))
    secret_64 = bytes(range(0x40, 0x80))

    kek_32 = derive_kek(secret_32, store_id)
    kek_64 = derive_kek(secret_64, store_id)

    print(f"store_id  = {store_id.hex()}")
    print(f"secret_32 = {secret_32.hex()}")
    print(f"kek_32    = {kek_32.hex()}")
    print(f"secret_64 = {secret_64.hex()}")
    print(f"kek_64    = {kek_64.hex()}")


if __name__ == "__main__":
    main()
