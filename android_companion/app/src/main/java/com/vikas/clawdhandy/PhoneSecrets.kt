package com.vikas.clawdhandy

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

object PhoneSecrets {
    private const val KEY_ALIAS = "clawd_anthropic_api_key"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"

    fun readApiKey(context: Context): String {
        val encoded = Store.prefs(context)
            .getString(Store.KEY_API, "").orEmpty()
        if (encoded.isBlank()) return ""
        val packed = Base64.decode(encoded, Base64.NO_WRAP)
        require(packed.size >= 29) { "Stored API key is invalid" }
        val iv = packed.copyOfRange(0, 12)
        val ciphertext = packed.copyOfRange(12, packed.size)
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, loadKey(), GCMParameterSpec(128, iv))
        return String(cipher.doFinal(ciphertext), Charsets.UTF_8)
    }

    fun saveApiKey(context: Context, apiKey: String) {
        val prefs = Store.prefs(context)
        if (apiKey.isBlank()) {
            prefs.edit().remove(Store.KEY_API).apply()
            return
        }
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, loadKey())
        val encrypted = cipher.doFinal(apiKey.toByteArray(Charsets.UTF_8))
        val packed = cipher.iv + encrypted
        prefs.edit()
            .putString(Store.KEY_API, Base64.encodeToString(packed, Base64.NO_WRAP))
            .apply()
    }

    private fun loadKey(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build()
        )
        return generator.generateKey()
    }
}
