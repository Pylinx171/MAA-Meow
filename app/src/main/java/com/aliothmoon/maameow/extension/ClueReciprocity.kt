package com.aliothmoon.maameow.extension

import android.content.Context
import android.os.Build
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.Checkbox
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import com.aliothmoon.maameow.R
import com.aliothmoon.maameow.data.model.InfrastConfig
import com.aliothmoon.maameow.maa.task.MaaTaskParams
import com.aliothmoon.maameow.maa.task.MaaTaskType
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.koin.core.context.GlobalContext
import java.io.IOException

object ClueReciprocity {
    private const val PREFERENCES = "clue_reciprocity"
    private const val CORE_MARKER = "pylinx/clue-reciprocity"

    fun supportsClient(clientType: String): Boolean = clientType == "Official" || clientType == "Bilibili"

    fun isAvailable(context: Context): Boolean = try {
        val abi = Build.SUPPORTED_ABIS.firstOrNull()
        abi != null && context.assets.open("$CORE_MARKER-$abi").use { true }
    } catch (_: IOException) {
        false
    }

    fun isEnabled(context: Context, taskId: String): Boolean =
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getBoolean(taskId, false)

    fun setEnabled(context: Context, taskId: String, enabled: Boolean) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .edit().putBoolean(taskId, enabled).apply()
    }

    fun taskParams(
        config: Any,
        taskId: String,
        profileId: String,
        clientType: String,
        task: MaaTaskParams,
    ): MaaTaskParams {
        if (config !is InfrastConfig || task.type != MaaTaskType.INFRAST ||
            profileId.isBlank() || !supportsClient(clientType)
        ) return task
        val context: Context = GlobalContext.getOrNull()?.getOrNull() ?: return task
        if (!isEnabled(context, taskId) || !isAvailable(context)) return task

        val params = Json.parseToJsonElement(task.params) as JsonObject
        val ledgerKey = "$profileId:$clientType".encodeToByteArray()
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        return task.copy(params = JsonObject(params + mapOf(
            "reception_clue_exchange" to JsonPrimitive(false),
            "reception_send_clue" to JsonPrimitive(false),
            "reception_clue_reciprocity" to JsonPrimitive(ledgerKey),
        )).toString())
    }
}

@Composable
fun ClueReciprocitySetting(taskId: String, clientType: String) {
    val context = LocalContext.current
    val hasCore = remember(context) { ClueReciprocity.isAvailable(context) }
    val available = hasCore && ClueReciprocity.supportsClient(clientType)
    var enabled by remember(context, taskId) {
        mutableStateOf(ClueReciprocity.isEnabled(context, taskId))
    }
    Row(
        modifier = Modifier.fillMaxWidth().toggleable(
            value = enabled,
            enabled = available,
            role = Role.Checkbox,
            onValueChange = {
                enabled = it
                ClueReciprocity.setEnabled(context, taskId, it)
            },
        ),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(checked = enabled, onCheckedChange = null, enabled = available)
        Column {
            Text(stringResource(R.string.pylinx_clue_reciprocity_title))
            Text(
                stringResource(
                    when {
                        available -> R.string.pylinx_clue_reciprocity_description
                        !hasCore -> R.string.pylinx_clue_reciprocity_unavailable
                        else -> R.string.pylinx_clue_reciprocity_unsupported_client
                    }
                ),
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}
