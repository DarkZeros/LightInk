package com.lightink.companion.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// E-ink: paper and ink, used when the phone has no dynamic colours
private val Light = lightColorScheme(
    primary = Color(0xFF1F1F1F),
    onPrimary = Color(0xFFF7F5F0),
    secondary = Color(0xFF5B5B5B),
    background = Color(0xFFF7F5F0),
    surface = Color(0xFFF7F5F0),
)
private val Dark = darkColorScheme(
    primary = Color(0xFFEDEBE6),
    onPrimary = Color(0xFF1F1F1F),
    secondary = Color(0xFFBDBDBD),
)

@Composable
fun LightInkTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) dynamicDarkColorScheme(LocalContext.current) else dynamicLightColorScheme(LocalContext.current)
        dark -> Dark
        else -> Light
    }
    MaterialTheme(colorScheme = colors, content = content)
}
