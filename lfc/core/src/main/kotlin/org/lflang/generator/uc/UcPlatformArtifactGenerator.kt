package org.lflang.generator.uc

/**
 * Contract for optional platform-specific artifact generators. Implementations emit additional
 * build scaffolding beyond the default native files.
 */
fun interface UcPlatformArtifactGenerator {
  fun generate()
}
