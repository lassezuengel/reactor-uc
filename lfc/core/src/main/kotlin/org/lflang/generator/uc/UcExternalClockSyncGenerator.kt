package org.lflang.generator.uc

import org.lflang.AttributeUtils
import org.lflang.lf.Reactor

class UcExternalClockSyncGenerator(
    private val reactor: Reactor,
    private val currentFederate: UcFederate? = null,
    private val allFederates: List<UcFederate> = emptyList()
) {

  companion object {
    const val numSystemEventsConst = 2
    const val instName = "external_clock_sync"

    fun getNumSystemEvents(): Int = numSystemEventsConst
  }

  private val attr = AttributeUtils.findAttributeByName(reactor, "external_clock_sync")
  private val modulePath = attr?.getParamString("module")?.trim()
  private val apiName = "external_clock_sync_api"
  private val isGrandmaster = currentFederate?.clockSyncParams?.grandmaster ?: false
  private val federateId =
      if (currentFederate != null) allFederates.indexOf(currentFederate) else -1

  fun enabled(): Boolean {
    return !modulePath.isNullOrBlank()
  }

  fun generateModuleInclude() = if (enabled()) "#include ${modulePath}" else ""

  fun generateApiStruct() =
      if (enabled())
          "static const ExternalClockSyncApi ${apiName} = { .init = lf_clock_sync_init, .schedule = lf_clock_sync_schedule };"
      else ""

  fun generateApiConfig() =
      if (enabled())
          "static const bool external_clock_sync_is_grandmaster = ${isGrandmaster};\n" +
              "static const int external_clock_sync_federate_id = ${federateId};"
      else ""

  fun generateSelfStruct() =
      if (enabled()) "LF_DEFINE_EXTERNAL_CLOCK_SYNC_STRUCT(Federate, ${numSystemEventsConst})"
      else ""

  fun generateCtor() =
      if (enabled())
          "LF_DEFINE_EXTERNAL_CLOCK_SYNC_CTOR(Federate, ${numSystemEventsConst}, ${apiName}, external_clock_sync_is_grandmaster, external_clock_sync_federate_id);"
      else ""

  fun generateFederateStructField() =
      if (enabled()) "FederateExternalClockSync ${instName};" else ""

  fun generateFederateCtorCode() =
      if (enabled()) "LF_INITIALIZE_EXTERNAL_CLOCK_SYNC(Federate);" else ""
}
