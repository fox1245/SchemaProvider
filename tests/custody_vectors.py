#!/usr/bin/env python3
"""Independent reference vectors for HKDF-SHA256 KEK derivation, custody store header layout, and sealed record framing using only the Python standard library."""

import hashlib
import hmac
import sys


def encode_u64(value: int) -> bytes:
    return value.to_bytes(8, "big")


def encode_text(value: bytes) -> bytes:
    return encode_u64(len(value)) + value


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


def build_epoch_aad(
    store_id: bytes,
    owner_scope: bytes,
    descriptor_identity: bytes,
    epoch: int,
    wrapping_key_id: bytes,
) -> bytes:
    assert len(store_id) == 32
    assert len(descriptor_identity) == 32
    return (
        encode_text(b"sp.native.archive.custody.epoch-aad.v1")
        + encode_u64(1)
        + store_id
        + encode_text(owner_scope)
        + descriptor_identity
        + encode_u64(epoch)
        + encode_text(wrapping_key_id)
    )


def derive_header_mac_key(dek_current: bytes) -> bytes:
    assert len(dek_current) == 32
    info = encode_text(b"sp.native.archive.custody.header-tag.v1")
    return hkdf_sha256_expand(dek_current, info, 32)


def build_record_aad(
    store_id: bytes,
    header_epoch: int,
    owner_scope: bytes,
    descriptor_identity: bytes,
    binding: bytes,
) -> bytes:
    assert len(store_id) == 32
    assert len(descriptor_identity) == 32
    return (
        encode_text(b"sp.native.archive.custody.record-aad.v1")
        + encode_u64(1)
        + store_id
        + encode_u64(header_epoch)
        + encode_text(owner_scope)
        + descriptor_identity
        + encode_text(binding)
    )


def build_record_plaintext_single_user_text(message_id: bytes, text_value: bytes) -> bytes:
    return (
        encode_u64(1)  # messages.size() == 1
        + encode_text(message_id)  # m.id
        + encode_u64(2)  # Role::User == 2
        + encode_u64(1)  # m.parts.size() == 1
        + encode_u64(0)  # Part tag 0 == Text
        + encode_text(text_value)  # Text::value
        + encode_u64(0)  # m.wire_output absent
        + encode_u64(0)  # m.native absent
    )


def build_record_envelope_prefix(
    header_epoch: int, record_nonce: bytes, ciphertext_len: int
) -> bytes:
    assert len(record_nonce) == 24
    return (
        encode_u64(1)
        + encode_u64(header_epoch)
        + record_nonce
        + encode_u64(ciphertext_len)
    )


def compute_record_reference(envelope_bytes: bytes) -> str:
    ref_domain_prefix = encode_text(b"sp.native.archive.custody.record-ref.v1")
    digest = hashlib.sha256(ref_domain_prefix + envelope_bytes).hexdigest()
    return f"spna4:{digest}"


def main(argv: list[str]) -> None:
    if len(argv) > 1:
        descriptor_id = bytes.fromhex(argv[1])
        if len(descriptor_id) != 32:
            raise ValueError("descriptor_identity hex argument must be 32 bytes (64 hex chars)")
    else:
        descriptor_id = b"\x00" * 32

    store_id = bytes(range(0x00, 0x20))
    secret_32 = bytes(range(0x20, 0x40))
    secret_64 = bytes(range(0x40, 0x80))
    dek_1 = bytes(range(0x80, 0xA0))
    nonce_1 = bytes(range(0xA0, 0xB8))
    record_nonce = bytes(range(0xB8, 0xD0))
    owner_scope = b"owner-golden"
    key_id = b"key-fixed"
    record_binding = b"binding-golden"

    kek_32 = derive_kek(secret_32, store_id)
    kek_64 = derive_kek(secret_64, store_id)

    epoch_aad_1 = build_epoch_aad(store_id, owner_scope, descriptor_id, 1, key_id)
    epoch_aad_prefix = epoch_aad_1[:106]
    epoch_aad_suffix = epoch_aad_1[138:]
    header_body_prefix = (
        encode_text(b"sp.native.archive.custody.header.v1")
        + encode_u64(1)
        + store_id
        + encode_text(owner_scope)
    )
    header_body_mid_and_nonce = (
        encode_u64(1)
        + encode_u64(1)
        + encode_u64(1)
        + encode_text(key_id)
        + encode_u64(72)
        + nonce_1
    )
    header_mac_key = derive_header_mac_key(dek_1)

    record_aad_1 = build_record_aad(store_id, 1, owner_scope, descriptor_id, record_binding)
    record_aad_prefix = record_aad_1[:115]
    record_aad_suffix = record_aad_1[147:]
    record_plaintext = build_record_plaintext_single_user_text(b"msg-1", b"hello")
    record_ciphertext_len = len(record_plaintext) + 16
    record_envelope_prefix = build_record_envelope_prefix(1, record_nonce, record_ciphertext_len)
    record_ref_domain_prefix = encode_text(b"sp.native.archive.custody.record-ref.v1")
    sample_envelope = record_envelope_prefix + bytes(range(record_ciphertext_len))
    sample_reference = compute_record_reference(sample_envelope)

    print(f"store_id                  = {store_id.hex()}")
    print(f"secret_32                 = {secret_32.hex()}")
    print(f"kek_32                    = {kek_32.hex()}")
    print(f"secret_64                 = {secret_64.hex()}")
    print(f"kek_64                    = {kek_64.hex()}")
    print(f"dek_1                     = {dek_1.hex()}")
    print(f"nonce_1                   = {nonce_1.hex()}")
    print(f"descriptor_identity       = {descriptor_id.hex()}")
    print(f"epoch_aad_prefix          = {epoch_aad_prefix.hex()}")
    print(f"epoch_aad_suffix          = {epoch_aad_suffix.hex()}")
    print(f"epoch_aad_1               = {epoch_aad_1.hex()}")
    print(f"header_body_prefix        = {header_body_prefix.hex()}")
    print(f"header_body_mid_and_nonce = {header_body_mid_and_nonce.hex()}")
    print(f"header_mac_key            = {header_mac_key.hex()}")
    print(f"record_nonce              = {record_nonce.hex()}")
    print(f"record_aad_prefix         = {record_aad_prefix.hex()}")
    print(f"record_aad_suffix         = {record_aad_suffix.hex()}")
    print(f"record_aad_1              = {record_aad_1.hex()}")
    print(f"record_plaintext          = {record_plaintext.hex()}")
    print(f"record_envelope_prefix    = {record_envelope_prefix.hex()}")
    print(f"record_ref_domain_prefix  = {record_ref_domain_prefix.hex()}")
    print(f"sample_reference          = {sample_reference}")


if __name__ == "__main__":
    main(sys.argv)

