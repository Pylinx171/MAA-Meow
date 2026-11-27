package com.aliothmoon.maameow.extension

import android.os.Build
import com.aliothmoon.maameow.BuildConfig
import com.aliothmoon.maameow.R
import com.aliothmoon.maameow.data.api.HttpClientHelper
import com.aliothmoon.maameow.data.datasource.ResourceDownloader
import com.aliothmoon.maameow.data.model.update.UpdateCheckResult
import com.aliothmoon.maameow.data.model.update.UpdateError
import com.aliothmoon.maameow.data.model.update.UpdateInfo
import com.aliothmoon.maameow.utils.i18n.LocalizedException
import com.aliothmoon.maameow.utils.i18n.uiTextOf
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.io.IOException
import java.net.URI
import java.security.MessageDigest

internal class PylinxUpdates(private val http: HttpClientHelper) {
    suspend fun checkApp(): UpdateCheckResult = check { index ->
        if (index.app.versionCode > BuildConfig.VERSION_CODE && index.app.abi in Build.SUPPORTED_ABIS) {
            UpdateCheckResult.Available(UpdateInfo(index.app.versionName, index.app.releaseNote))
        } else {
            UpdateCheckResult.UpToDate(BuildConfig.VERSION_NAME)
        }
    }

    suspend fun checkResources(current: String): UpdateCheckResult = check { index ->
        if (index.resource.core == BuildConfig.MAA_CORE_VERSION &&
            ResourceDownloader.compareVersions(index.resource.version, current) > 0
        ) {
            UpdateCheckResult.Available(UpdateInfo(ResourceDownloader.formatVersionForDisplay(index.resource.version)))
        } else {
            UpdateCheckResult.UpToDate(current)
        }
    }

    suspend fun appAsset(version: String): Result<UpdateAsset> = fetchIndex().mapCatching { index ->
        index.app.asset.takeIf {
            index.app.versionName == version && index.app.versionCode > BuildConfig.VERSION_CODE &&
                index.app.abi in Build.SUPPORTED_ABIS
        } ?: throw LocalizedException(uiTextOf(R.string.pylinx_update_index_changed))
    }

    suspend fun resourceAsset(current: String): Result<UpdateAsset> = fetchIndex().mapCatching { index ->
        index.resource.asset.takeIf {
            index.resource.core == BuildConfig.MAA_CORE_VERSION &&
                ResourceDownloader.compareVersions(index.resource.version, current) > 0
        } ?: throw LocalizedException(uiTextOf(R.string.pylinx_update_index_changed))
    }

    private suspend fun check(onSuccess: (Index) -> UpdateCheckResult): UpdateCheckResult =
        fetchIndex().fold(onSuccess) { e ->
            UpdateCheckResult.Error(
                if (e is LocalizedException) UpdateError.UnknownError(e.uiText)
                else UpdateError.NetworkError(e.message),
            )
        }

    private suspend fun fetchIndex(): Result<Index> = withContext(Dispatchers.IO) {
        runCatching {
            http.get(INDEX_URL, headers = mapOf("Cache-Control" to "no-cache")).use { response ->
                if (!response.isSuccessful) {
                    throw LocalizedException(uiTextOf(R.string.update_error_http_status, response.code))
                }
                parseIndex(response.body.string())
            }
        }.onFailure { if (it is CancellationException) throw it }
    }

    @Serializable
    internal data class Index(val schema: Int, val app: App, val resource: Resource)

    @Serializable
    internal data class App(
        val versionName: String,
        val versionCode: Int,
        val abi: String,
        val asset: UpdateAsset,
        val releaseNote: String? = null,
    )

    @Serializable
    internal data class Resource(val core: String, val version: String, val asset: UpdateAsset)

    companion object {
        const val INDEX_URL = "https://raw.githubusercontent.com/Pylinx171/MAA-Meow/pylinx-updates/update.json"
        const val RELEASES_URL = "https://github.com/Pylinx171/MAA-Meow/releases"
        private val json = Json { ignoreUnknownKeys = true }

        internal fun parseIndex(content: String): Index {
            val index = json.decodeFromString<Index>(content)
            require(index.schema == 1 && index.app.versionCode > 0 && index.app.abi == "arm64-v8a")
            require(Regex("""\d+\.\d+\.\d+""").matches(index.app.versionName))
            require(Regex("""v\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?""").matches(index.resource.core))
            require(Regex("""\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}""").matches(index.resource.version))
            require(validAsset(index.app.asset, ".apk") && validAsset(index.resource.asset, ".zip"))
            return index
        }

        private fun validAsset(asset: UpdateAsset, suffix: String): Boolean {
            val uri = URI(asset.url)
            return uri.scheme == "https" && uri.host == "github.com" && uri.port == -1 &&
                uri.rawUserInfo == null && uri.rawQuery == null && uri.rawFragment == null &&
                uri.rawPath.startsWith("/Pylinx171/MAA-Meow/releases/download/") &&
                uri.rawPath.endsWith(suffix) && uri.normalize() == uri &&
                asset.sha256?.matches(Regex("[0-9a-f]{64}")) == true
        }
    }
}

@Serializable
internal data class UpdateAsset(val url: String, val sha256: String? = null) {
    suspend fun verifies(file: File): Boolean {
        val expected = sha256 ?: return true
        return withContext(Dispatchers.IO) {
            try {
                val hash = MessageDigest.getInstance("SHA-256")
                file.inputStream().buffered().use { input ->
                    val buffer = ByteArray(64 * 1024)
                    while (true) {
                        ensureActive()
                        val length = input.read(buffer)
                        if (length < 0) break
                        hash.update(buffer, 0, length)
                    }
                }
                ensureActive()
                hash.digest().toHexString() == expected
            } catch (_: IOException) {
                false
            }
        }
    }
}
