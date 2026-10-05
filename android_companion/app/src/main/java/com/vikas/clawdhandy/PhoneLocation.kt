package com.vikas.clawdhandy

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.Geocoder
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Looper
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/** Where the phone is, for the handheld's weather, planes and "near me" questions. */
object PhoneLocation {
    data class Fix(val lat: Double, val lon: Double, val accuracy: Float, val city: String, val at: Long)

    @Volatile private var cached: Fix? = null
    private val geocodeWorker = Executors.newSingleThreadExecutor()

    fun hasPermission(context: Context) =
        context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED ||
            context.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED

    /** The last fix we got, even if old (null if never). */
    fun last(context: Context): Fix? = cached ?: loadSaved(context)?.also { cached = it }

    /**
     * A fix no older than [maxAgeMs], asking the GPS for a new one if needed (waits up to
     * [waitMs]). Blocking: call it off the main thread. Falls back to the last fix we had.
     */
    @SuppressLint("MissingPermission")
    fun current(context: Context, maxAgeMs: Long = 120_000, waitMs: Long = 10_000): Fix? {
        val app = context.applicationContext
        cached?.let { if (System.currentTimeMillis() - it.at < maxAgeMs) return it }
        if (!hasPermission(app)) return last(app)
        val manager = app.getSystemService(LocationManager::class.java) ?: return last(app)
        val providers = manager.getProviders(true)
        val recent = providers.mapNotNull { runCatching { manager.getLastKnownLocation(it) }.getOrNull() }
            .filter { System.currentTimeMillis() - it.time < maxAgeMs }
            .minByOrNull { it.accuracy }
        val location = recent ?: requestFresh(manager, providers, waitMs)
        if (location == null) return last(app)
        val fix = Fix(location.latitude, location.longitude, location.accuracy, cityFor(app, location), System.currentTimeMillis())
        cached = fix
        save(app, fix)
        return fix
    }

    @SuppressLint("MissingPermission")
    private fun requestFresh(manager: LocationManager, enabled: List<String>, waitMs: Long): Location? {
        val provider = when {
            Build.VERSION.SDK_INT >= 31 && LocationManager.FUSED_PROVIDER in enabled -> LocationManager.FUSED_PROVIDER
            LocationManager.NETWORK_PROVIDER in enabled -> LocationManager.NETWORK_PROVIDER // fast, works indoors
            LocationManager.GPS_PROVIDER in enabled -> LocationManager.GPS_PROVIDER
            else -> return null
        }
        val done = CountDownLatch(1)
        var result: Location? = null
        try {
            if (Build.VERSION.SDK_INT >= 30) {
                manager.getCurrentLocation(provider, null, geocodeWorker) { location ->
                    result = location
                    done.countDown()
                }
            } else {
                val listener = object : LocationListener {
                    override fun onLocationChanged(location: Location) {
                        result = location
                        done.countDown()
                    }
                    @Deprecated("Deprecated in Android")
                    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) = Unit
                    override fun onProviderEnabled(provider: String) = Unit
                    override fun onProviderDisabled(provider: String) = Unit
                }
                @Suppress("DEPRECATION")
                manager.requestSingleUpdate(provider, listener, Looper.getMainLooper())
                if (!done.await(waitMs, TimeUnit.MILLISECONDS)) manager.removeUpdates(listener)
                return result
            }
            done.await(waitMs, TimeUnit.MILLISECONDS)
        } catch (_: SecurityException) {
        } catch (_: IllegalArgumentException) {
        }
        return result
    }

    private fun cityFor(context: Context, location: Location): String = runCatching {
        if (!Geocoder.isPresent()) return ""
        @Suppress("DEPRECATION")
        val place = Geocoder(context, Locale.US).getFromLocation(location.latitude, location.longitude, 1)?.firstOrNull()
        val name = place?.locality ?: place?.subAdminArea ?: place?.adminArea ?: ""
        BoardText.ascii(name)
    }.getOrDefault("")

    private fun save(context: Context, fix: Fix) {
        Store.prefs(context).edit()
            .putString("loc_lat", fix.lat.toString()).putString("loc_lon", fix.lon.toString())
            .putFloat("loc_acc", fix.accuracy).putString("loc_city", fix.city).putLong("loc_at", fix.at)
            .apply()
    }

    private fun loadSaved(context: Context): Fix? {
        val prefs = Store.prefs(context)
        val lat = prefs.getString("loc_lat", null)?.toDoubleOrNull() ?: return null
        val lon = prefs.getString("loc_lon", null)?.toDoubleOrNull() ?: return null
        return Fix(lat, lon, prefs.getFloat("loc_acc", 0f), prefs.getString("loc_city", "") ?: "", prefs.getLong("loc_at", 0))
    }
}
