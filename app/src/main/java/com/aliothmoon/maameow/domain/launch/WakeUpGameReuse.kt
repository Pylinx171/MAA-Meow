package com.aliothmoon.maameow.domain.launch

import com.aliothmoon.maameow.RemoteService
import com.aliothmoon.maameow.domain.models.RunMode
import com.aliothmoon.maameow.maa.task.MaaTaskParams
import com.aliothmoon.maameow.maa.task.MaaTaskType
import com.aliothmoon.maameow.manager.RemoteCallStuckException
import com.aliothmoon.maameow.manager.RemoteServiceManager
import com.aliothmoon.maameow.remote.AppAliveStatus
import com.aliothmoon.maameow.remote.internal.PrimaryDisplayManager
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.jsonObject

internal suspend fun prepareRunningWakeUpGame(
    tasks: List<MaaTaskParams>,
    service: RemoteService,
    packageName: String,
    mode: RunMode,
    onAbandoned: (RemoteCallStuckException) -> Unit,
): List<MaaTaskParams>? {
    suspend fun <T> call(name: String, timeout: Long = 15_000, block: (RemoteService) -> T): T =
        RemoteServiceManager.callOrRestart(service, name, timeout, onAbandoned, block)

    return reuseRunningWakeUpGame(
        tasks,
        isGameRunning = { call("isAppAlive") { it.isAppAlive(packageName) } == AppAliveStatus.ALIVE },
        showRunningGame = {
            if (mode == RunMode.BACKGROUND) {
                call("isAppOnVirtualDisplay") { it.isAppOnVirtualDisplay(packageName) } ||
                    call("moveAppToVirtualDisplay") { it.moveAppToVirtualDisplay(packageName) }
            } else {
                call("getVisiblePackages") {
                    it.getVisiblePackages(PrimaryDisplayManager.DISPLAY_ID)?.contains(packageName)
                } == true || call("launchGame", 30_000) { it.launchGame(packageName) }
            }
        },
    )
}

/**
 * 游戏已运行时只禁用 MaaCore 唤醒任务的启动动作，保留账号切换和进入首页的流程。
 * 返回 null 表示已有游戏无法放到目标屏，调用方应中止本轮。
 */
internal suspend fun reuseRunningWakeUpGame(
    tasks: List<MaaTaskParams>,
    isGameRunning: suspend () -> Boolean,
    showRunningGame: suspend () -> Boolean,
): List<MaaTaskParams>? {
    val startupParams = tasks.map { task ->
        if (task.type == MaaTaskType.START_UP) {
            Json.parseToJsonElement(task.params).jsonObject.takeIf {
                (it["start_game_enabled"] as? JsonPrimitive)?.booleanOrNull == true
            }
        } else {
            null
        }
    }
    if (startupParams.all { it == null } || !isGameRunning()) return tasks
    if (!showRunningGame()) return null

    return tasks.mapIndexed { index, task ->
        val params = startupParams[index] ?: return@mapIndexed task
        task.copy(params = JsonObject(params + ("start_game_enabled" to JsonPrimitive(false))).toString())
    }
}
