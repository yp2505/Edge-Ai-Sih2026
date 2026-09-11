// AES-128-CTR encryption for WiFi credentials (matches server.py key)
// Uses Web Crypto API (available in all modern browsers)

const AES_KEY_HEX = import.meta.env.VITE_AES_KEY || '48657956616e6e69534948323032362a'

function hexToBytes(hex) {
  const bytes = new Uint8Array(hex.length / 2)
  for (let i = 0; i < hex.length; i += 2) {
    bytes[i / 2] = parseInt(hex.substring(i, i + 2), 16)
  }
  return bytes
}

function bytesToHex(bytes) {
  return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('')
}

export async function encryptPassword(password) {
  if (!password) return ''

  const keyBytes = hexToBytes(AES_KEY_HEX)
  const cryptoKey = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'AES-CTR' }, false, ['encrypt']
  )

  const nonce = crypto.getRandomValues(new Uint8Array(16))
  const plaintext = new TextEncoder().encode(password)

  const ciphertext = await crypto.subtle.encrypt(
    { name: 'AES-CTR', counter: nonce, length: 64 },
    cryptoKey, plaintext
  )

  return bytesToHex(nonce) + ':' + bytesToHex(new Uint8Array(ciphertext))
}

export async function decryptPassword(encrypted) {
  if (!encrypted || !encrypted.includes(':')) return encrypted

  const [nonceHex, cipherHex] = encrypted.split(':')
  const nonce = hexToBytes(nonceHex)
  const ciphertext = hexToBytes(cipherHex)

  const keyBytes = hexToBytes(AES_KEY_HEX)
  const cryptoKey = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'AES-CTR' }, false, ['decrypt']
  )

  const plainBuffer = await crypto.subtle.decrypt(
    { name: 'AES-CTR', counter: nonce, length: 64 },
    cryptoKey, ciphertext
  )

  return new TextDecoder().decode(plainBuffer)
}
