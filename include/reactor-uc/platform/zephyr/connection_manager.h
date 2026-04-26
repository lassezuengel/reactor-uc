/**
 * @brief Zephyr network connection manager for federated startup.
 *
 * This module provides a small synchronization API used by federated programs
 * on Zephyr:
 * - `lf_init_connection_manager(ipv6_addr)` configures 802.15.4 MAC (if applicable)
 *   and registers network-state monitoring.
 * - `lf_wait_for_network_connection()` blocks until network readiness is signaled.
 *
 * This is useful (and possibly needed) on targets where networking is not immediately available after
 * boot (for example, IPv6 over IEEE 802.15.4 / 6LoWPAN).
 */

#ifndef REACTOR_UC_PLATFORM_ZEPHYR_CONNECTION_MANAGER_H
#define REACTOR_UC_PLATFORM_ZEPHYR_CONNECTION_MANAGER_H

#ifdef PLATFORM_ZEPHYR
/**
 * @brief Initialize connection monitoring for federated startup.
 * @ingroup Federated
 *
 * Call this during startup before entering federated execution. For IEEE 802.15.4
 * deployments, pass the compile-time IPv6 address to derive a matching EUI-64.
 * After
 * initialization, call `lf_wait_for_network_connection()` to wait until the
 * local network is ready.
 *
 * @param ipv6_addr Compile-time IPv6 address used to derive EUI-64, or NULL.
 */
void lf_init_connection_manager(const char* ipv6_addr);

/**
 * @brief Block until a local network connection is available.
 * @ingroup Federated
 *
 * This function waits indefinitely until readiness is signaled by the
 * connection manager.
 */
void lf_wait_for_network_connection(void);

/**
 * @brief Set the IPv6 address for the default network interface.
 * @ingroup Federated
 *
 * Call this after lf_wait_for_network_connection() to configure the
 * IPv6 address. The address string should be in standard IPv6 notation
 * (e.g., "fe80::1").
 *
 * @param ipv6_addr The IPv6 address string to set
 * @return 0 on success, negative error code on failure
 */
int lf_set_ipv6_address(const char* ipv6_addr);
#endif // PLATFORM_ZEPHYR

#endif