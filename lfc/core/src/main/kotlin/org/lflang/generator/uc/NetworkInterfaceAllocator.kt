package org.lflang.generator.uc

import org.lflang.AttributeUtils
import org.lflang.lf.Attribute

/** Provides IP addresses for network interfaces while hiding platform-specific allocation rules. */
interface NetworkInterfaceAllocator {
  val usesIpv6: Boolean

  /** Prepare allocator state before any interfaces are created. */
  fun initialize(federates: List<UcFederate>)

  /**
   * Return an address for the given federate/interface attribute. [fallback] supplies the default
   * address when no explicit value is provided.
   */
  fun allocateAddress(
      federate: UcFederate,
      attribute: Attribute?,
      fallback: () -> IPAddress
  ): IPAddress
}

/**
 * A network interface allocator for IPv4 addresses. This allocator does not support address
 * reservation, so it assumes that any explicitly specified address is valid and available. For
 * federates that are part of a bank, the allocator will apply a simple offset to the specified
 * address to derive unique addresses for each bank member. If no explicit address is provided, the
 * allocator will return the loopback address.
 */
class Ipv4NetworkInterfaceAllocator : NetworkInterfaceAllocator {
  override val usesIpv6: Boolean = false

  override fun initialize(federates: List<UcFederate>) {
    // Nothing to prepare for IPv4. All addresses come directly from attributes or loopback.
  }

  override fun allocateAddress(
      federate: UcFederate,
      attribute: Attribute?,
      fallback: () -> IPAddress
  ): IPAddress {
    val literal = attribute?.getParamString("address")
    if (literal != null) {
      val parsed =
          try {
            IPAddress.fromString(literal)
          } catch (ex: IllegalArgumentException) {
            throw NetworkInterfaceAllocationException(
                federate, attribute, ex.message ?: "Invalid IPv4 address: $literal", ex)
          }
      val ipv4 =
          parsed as? IPAddress.IPv4
              ?: throw NetworkInterfaceAllocationException(
                  federate, attribute, "Expected IPv4 address but got $literal.")
      val adjusted =
          try {
            if (federate.isBank) IPAddress.increment(ipv4, federate.bankIdx - 1) else ipv4
          } catch (ex: IllegalArgumentException) {
            throw NetworkInterfaceAllocationException(
                federate,
                attribute,
                "Unable to derive IPv4 address for ${federate.name}: ${ex.message}",
                ex)
          }
      val adjustedIpv4 = adjusted as IPAddress.IPv4
      if (adjustedIpv4.address != LOOPBACK_IPV4.address) {
        try {
          IpAddressManager.acquireIp(adjustedIpv4)
        } catch (ex: IllegalArgumentException) {
          throw NetworkInterfaceAllocationException(
              federate, attribute, "IPv4 address ${adjustedIpv4.address} is already reserved.", ex)
        }
      }
      return adjustedIpv4
    }
    return fallback()
  }

  companion object {
    private val LOOPBACK_IPV4 = IPAddress.fromString("127.0.0.1") as IPAddress.IPv4
  }
}

/**
 * A network interface allocator for IPv6 addresses. This allocator supports reservation of
 * explicitly specified addresses to prevent conflicts. This is needed because, for example, the
 * ´sicslowpan´ net-interface requires unique IPv6 addresses for each federate, and the allocator
 * must ensure that these addresses do not conflict with each other or with automatically allocated
 * addresses.
 */
class Ipv6NetworkInterfaceAllocator : NetworkInterfaceAllocator {
  override val usesIpv6: Boolean = true

  private val reservations = mutableMapOf<Pair<UcFederate, Attribute?>, IPAddress.IPv6>()
  private val addressOwners = mutableMapOf<IPAddress.IPv6, Pair<UcFederate, Attribute?>>()

  override fun initialize(federates: List<UcFederate>) {
    reservations.clear()
    addressOwners.clear()
    IpAddressManager.resetIpv6Allocator()
    federates.forEach { federate ->
      AttributeUtils.getInterfaceAttributes(federate.inst).forEach { attr ->
        attr.getParamString("address")?.let { literal ->
          val ipv6 = adjustedIpv6(literal, federate, attr)
          registerReservation(federate, attr, ipv6)
        }
      }
    }
  }

  override fun allocateAddress(
      federate: UcFederate,
      attribute: Attribute?,
      _fallback: () -> IPAddress
  ): IPAddress {
    val key = reservationKey(federate, attribute)
    reservations[key]?.let {
      return it
    }
    attribute?.getParamString("address")?.let { literal ->
      val explicit = adjustedIpv6(literal, federate, attribute)
      registerReservation(federate, attribute, explicit)
      return explicit
    }
    val auto = IpAddressManager.acquireNextIpv6Address() as IPAddress.IPv6
    reservations[key] = auto
    addressOwners[auto] = federate to attribute
    return auto
  }

  private fun adjustedIpv6(
      literal: String,
      federate: UcFederate,
      attribute: Attribute?
  ): IPAddress.IPv6 {
    val parsed =
        try {
          IPAddress.fromString(literal)
        } catch (ex: IllegalArgumentException) {
          throw NetworkInterfaceAllocationException(
              federate, attribute, ex.message ?: "Invalid IPv6 address: $literal", ex)
        }
    val ipv6 =
        parsed as? IPAddress.IPv6
            ?: throw NetworkInterfaceAllocationException(
                federate,
                attribute,
                "interface address $literal must be IPv6 when sicslowpan is enabled for federate ${federate.name}.")
    val adjusted =
        try {
          if (federate.isBank) IPAddress.increment(ipv6, federate.bankIdx - 1) else ipv6
        } catch (ex: IllegalArgumentException) {
          throw NetworkInterfaceAllocationException(
              federate,
              attribute,
              "Unable to derive IPv6 address for ${federate.name}: ${ex.message}",
              ex)
        }
    return adjusted as IPAddress.IPv6
  }

  private fun registerReservation(
      federate: UcFederate,
      attribute: Attribute?,
      address: IPAddress.IPv6
  ) {
    addressOwners[address]?.let { (ownerFed, ownerAttr) ->
      if (ownerFed != federate || ownerAttr != attribute) {
        val existingLabel = describeOwner(ownerFed, ownerAttr)
        val newLabel = describeOwner(federate, attribute)
        throw NetworkInterfaceAllocationException(
            federate,
            attribute,
            "IPv6 address ${address.address} is already reserved by $existingLabel and cannot be reused by $newLabel.")
      } else {
        return
      }
    }
    try {
      IpAddressManager.acquireIp(address)
    } catch (ex: IllegalArgumentException) {
      throw NetworkInterfaceAllocationException(
          federate,
          attribute,
          "IPv6 address ${address.address} is already reserved by another interface.",
          ex)
    }
    reservations[reservationKey(federate, attribute)] = address
    addressOwners[address] = federate to attribute
  }

  private fun reservationKey(federate: UcFederate, attribute: Attribute?) = federate to attribute

  private fun describeOwner(federate: UcFederate, attribute: Attribute?): String {
    val attrLabel = attribute?.attrName ?: "default interface"
    return "federate ${federate.name} (@$attrLabel)"
  }
}

/** Indicates a failure while assigning network interface addresses for a federate. */
public class NetworkInterfaceAllocationException(
    val federate: UcFederate?,
    val attribute: Attribute?,
    message: String,
    cause: Throwable? = null
) : RuntimeException(message, cause)
