package com.aliothmoon.maameow.extension

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class PylinxUpdatesTest {
    @get:Rule val temporaryFolder = TemporaryFolder()

    private val hash = "a".repeat(64)
    private val manifest = """
        {
          "schema":1,
          "app":{"versionName":"0.28.2","versionCode":1011,"abi":"arm64-v8a",
                 "asset":{"url":"https://github.com/Pylinx171/MAA-Meow/releases/download/pylinx-v0.28.2-1011/app.apk","sha256":"$hash"}},
          "resource":{"core":"v6.23.2","version":"2026-11-23 12:34:56.000",
                 "asset":{"url":"https://github.com/Pylinx171/MAA-Meow/releases/download/pylinx-v0.28.2-1011/resource.zip","sha256":"$hash"}}
        }
    """.trimIndent()

    @Test
    fun forkIndexContainsExpectedVersions() {
        val index = PylinxUpdates.parseIndex(manifest)
        assertEquals(1011, index.app.versionCode)
        assertEquals("v6.23.2", index.resource.core)
        assertEquals(hash, index.resource.asset.sha256)
    }

    @Test
    fun indexRejectsAnUnexpectedSourceOrChecksum() {
        assertThrows(IllegalArgumentException::class.java) {
            PylinxUpdates.parseIndex(manifest.replace("Pylinx171/MAA-Meow", "Aliothmoon/MAA-Meow"))
        }
        assertThrows(IllegalArgumentException::class.java) {
            PylinxUpdates.parseIndex(manifest.replace(hash, "incorrect"))
        }
    }

    @Test
    fun hashCheckRejectsChangedBytes() = runBlocking {
        val file = temporaryFolder.newFile()
        file.writeText("abc")
        val asset = UpdateAsset("unused", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        assertTrue(asset.verifies(file))
        file.writeText("changed")
        assertFalse(asset.verifies(file))
    }
}
