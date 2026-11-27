package com.aliothmoon.maameow.domain.launch

import com.aliothmoon.maameow.maa.task.MaaTaskParams
import com.aliothmoon.maameow.maa.task.MaaTaskType
import com.aliothmoon.maameow.maa.task.TaskSlot
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Test

class WakeUpGameReuseTest {
    private val startup = MaaTaskParams(
        MaaTaskType.START_UP,
        """{"client_type":"Official","start_game_enabled":true,"account_name":"123****4567"}""",
        TaskSlot("startup", 0),
    )
    private val award = MaaTaskParams(MaaTaskType.AWARD, "{}")

    @Test
    fun runningGameOnlyDisablesLaunchingForThisRun() = runBlocking {
        val result = checkNotNull(reuseRunningWakeUpGame(listOf(startup, award), { true }, { true }))
        val actual = Json.parseToJsonElement(result.first().params).jsonObject
        val original = Json.parseToJsonElement(startup.params).jsonObject
        assertEquals(original + ("start_game_enabled" to JsonPrimitive(false)), actual)
        assertEquals(startup.slot, result.first().slot)
        assertSame(award, result.last())
    }

    @Test
    fun gameThatIsNotRunningKeepsStartupEnabled() = runBlocking {
        val tasks = listOf(startup, award)
        assertSame(tasks, reuseRunningWakeUpGame(tasks, { false }, { error("Cannot show a stopped game") }))
    }

    @Test
    fun launchDisabledDoesNotProbeOrBringTheGameForward() = runBlocking {
        val disabled = startup.copy(params = """{"client_type":"Official","start_game_enabled":false}""")
        val tasks = listOf(disabled, award)
        assertSame(tasks, reuseRunningWakeUpGame(tasks, { error("Unexpected probe") }, { error("Unexpected launch") }))
    }

    @Test
    fun failureToShowExistingGameStopsTheRun() = runBlocking {
        assertNull(reuseRunningWakeUpGame(listOf(startup, award), { true }, { false }))
    }
}
