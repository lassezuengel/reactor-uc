package org.lflang.generator.uc

import java.net.InetAddress
import kotlin.math.max

/**
 * Simple allocator for IPv6 addresses used by Zephyr federates. Generates addresses in the
 * fd01::/64 prefix and tracks reservations from attributes to avoid collisions across federates.
 */
object UcZephyrIpv6Allocator {
  private const val PREFIX = "fd01::"
  private var nextSuffix = 1
  private val reserved = mutableSetOf<String>()

  @Synchronized
  fun reset() {
    nextSuffix = 1
    reserved.clear()
  }

  @Synchronized
  fun nextAddress(): IPAddress {
    while (true) {
      val candidate = IPAddress.fromString("$PREFIX" + nextSuffix.toString(16))
      nextSuffix += 1
      val normalized = normalize(candidate.address)
      if (reserved.add(normalized)) {
        return candidate
      }
    }
  }

  @Synchronized
  fun markAsUsed(ip: IPAddress) {
    if (ip !is IPAddress.IPv6) {
      return
    }
    val normalized = normalize(ip.address)
    reserved.add(normalized)
    adjustNextSuffix(normalized)
  }

  private fun adjustNextSuffix(address: String) {
    val segments = address.split(":")
    if (segments.size != 8) {
      return
    }
    if (!segments.first().equals("fd01", ignoreCase = true)) {
      return
    }
    val suffix = segments.last().ifEmpty { "0" }
    val value = suffix.toIntOrNull(16) ?: return
    nextSuffix = max(nextSuffix, value + 1)
  }

  private fun normalize(address: String): String {
    return try {
      InetAddress.getByName(address).hostAddress.lowercase()
    } catch (e: Exception) {
      address.lowercase()
    }
  }
}
