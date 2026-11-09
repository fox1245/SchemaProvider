// JavaScript handoff helper for WebAssembly host archive custody (`sp::PreResolvedSecretSource`).
//
// Security and lifetime rules:
//  * Keys and secrets must never be JavaScript strings (V8 strings are immutable, interned, and
//    cannot be zeroed by the caller). Pass secret material only as a `Uint8Array`.
//  * Key identifiers cross the WebAssembly boundary as `(ptr, len)` byte spans, never NUL-terminated C strings.
//  * Always index `module.HEAPU8` after any `await` and immediately before `.set(...)` so that a
//    WebAssembly memory growth cannot leave a detached or stale `Uint8Array` view.
//  * Both the caller's `Uint8Array` and the WebAssembly transfer region are zeroed in `finally`.
//  * After a WebAssembly trap (`WebAssembly.RuntimeError`), C++ destructors and memory wipes did
//    not run; the host must discard the WebAssembly module instance and its linear memory.

const textEncoder = new TextEncoder();

function normalizeKeyId(keyId) {
  if (keyId instanceof Uint8Array) {
    return keyId;
  }
  if (typeof keyId === 'string') {
    return textEncoder.encode(keyId);
  }
  throw new TypeError('keyId must be a Uint8Array or string');
}

function faultReason(module, faultCode) {
  return module.ccall('sp_custody_fault_reason', 'string', ['number'], [faultCode]);
}

// The numeric code of an invalid key, as the C++ enum defines it.
function invalidKeyMaterialCode(module) {
  return module.ccall('sp_custody_fault_code_invalid_key_material', 'number', [], []);
}

export function createSecretSource(module) {
  const sourcePtr = module.ccall('sp_custody_secret_source_create', 'number', [], []) >>> 0;
  if (sourcePtr === 0) {
    throw new Error('sp_custody_secret_source_create failed');
  }
  return sourcePtr;
}

export function destroySecretSource(module, sourcePtr) {
  if (sourcePtr) {
    module.ccall('sp_custody_secret_source_destroy', null, ['number'], [sourcePtr]);
  }
}

export function setActiveKeyId(module, sourcePtr, keyId) {
  const keyIdBytes = normalizeKeyId(keyId);
  if (!sourcePtr || keyIdBytes.byteLength === 0) {
    return { ok: false, reason: faultReason(module, invalidKeyMaterialCode(module)) };
  }
  const fault = module.ccall(
    'sp_custody_secret_source_set_active_key_id',
    'number',
    ['number', 'array', 'number'],
    [sourcePtr, keyIdBytes, keyIdBytes.byteLength]
  );
  if (fault !== 0) {
    return { ok: false, reason: faultReason(module, fault) };
  }
  return { ok: true };
}

export async function handoffSecret(module, sourcePtr, keyId, secretInput, options = {}) {
  if (typeof secretInput === 'string') {
    throw new TypeError('custody secret must be a Uint8Array; keys must never be JavaScript strings');
  }
  const resolved = typeof secretInput === 'function' ? await secretInput() : await secretInput;
  if (!(resolved instanceof Uint8Array)) {
    throw new TypeError('custody secret must be a Uint8Array; keys must never be JavaScript strings');
  }
  const secretBytes = resolved;
  const byteLength = secretBytes.byteLength;
  let ptr = 0;
  let committed = false;
  try {
    const keyIdBytes = normalizeKeyId(keyId);
    if (!sourcePtr || keyIdBytes.byteLength === 0) {
      return {
        ok: false,
        reason: faultReason(module, invalidKeyMaterialCode(module)),
        ptr: 0,
        byteLength,
        transferOffset: 0,
        transferLength: 0,
      };
    }
    ptr = module.ccall('sp_custody_secret_alloc', 'number', ['number'], [byteLength]) >>> 0;
    if (ptr === 0) {
      return {
        ok: false,
        reason: faultReason(module, invalidKeyMaterialCode(module)),
        ptr: 0,
        byteLength,
        transferOffset: 0,
        transferLength: 0,
      };
    }
    if (typeof options.beforeCopy === 'function') {
      await options.beforeCopy(ptr, byteLength);
    }
    // Re-read module.HEAPU8 after any await or memory growth before copying the secret bytes.
    module.HEAPU8.set(secretBytes, ptr);
    const fault = module.ccall(
      'sp_custody_secret_commit',
      'number',
      ['number', 'array', 'number', 'number', 'number'],
      [sourcePtr, keyIdBytes, keyIdBytes.byteLength, ptr, byteLength]
    );
    committed = true;
    if (fault !== 0) {
      return {
        ok: false,
        reason: faultReason(module, fault),
        ptr,
        byteLength,
        transferOffset: ptr,
        transferLength: byteLength,
      };
    }
    if (options.active === true) {
      const activeFault = module.ccall(
        'sp_custody_secret_source_set_active_key_id',
        'number',
        ['number', 'array', 'number'],
        [sourcePtr, keyIdBytes, keyIdBytes.byteLength]
      );
      if (activeFault !== 0) {
        return {
          ok: false,
          reason: faultReason(module, activeFault),
          ptr,
          byteLength,
          transferOffset: ptr,
          transferLength: byteLength,
        };
      }
    }
    return {
      ok: true,
      ptr,
      byteLength,
      transferOffset: ptr,
      transferLength: byteLength,
    };
  } finally {
    secretBytes.fill(0);
    // After sp_custody_secret_commit the block belongs to C++, which wiped and freed it: never touch it again here.
    // Before a commit the block is still ours: wipe it, then hand it back.
    if (ptr !== 0 && !committed) {
      module.HEAPU8.fill(0, ptr, ptr + byteLength);
      module.ccall('sp_custody_secret_free', null, ['number', 'number'], [ptr, byteLength]);
    }
  }
}
