#!/usr/bin/env bash

set -euo pipefail

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:-auto}"
aptCmd=()

info() {
  printf '[INFO] %s\n' "$1"
}

error() {
  printf '[ERROR] %s\n' "$1" >&2
}

requireLinuxDebian() {
  if [[ "$(uname -s)" != "Linux" ]]; then
    error "Este script está pensado para Linux."
    exit 1
  fi

  if [[ ! -f /etc/os-release ]]; then
    error "No se pudo detectar la distribución."
    exit 1
  fi

  # shellcheck disable=SC1091
  source /etc/os-release

  if [[ "${ID:-}" != "debian" && "${ID_LIKE:-}" != *"debian"* ]]; then
    error "Este setup.sh está hecho para Debian o derivadas compatibles con apt."
    exit 1
  fi

  if ! command -v apt-get >/dev/null 2>&1; then
    error "No se encontró apt-get en el sistema."
    exit 1
  fi
}

configureAptCommand() {
  if [[ "$(id -u)" -eq 0 ]]; then
    aptCmd=(apt-get)
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    aptCmd=(sudo apt-get)
    return
  fi

  error "Se necesita ejecutar como root o tener sudo disponible para instalar dependencias."
  exit 1
}

installDependencies() {
  info "Actualizando índices de paquetes..."
  "${aptCmd[@]}" update

  info "Instalando dependencias de compilación..."
  "${aptCmd[@]}" install -y build-essential
}

checkMakefile() {
  local relativeDir="$1"

  if [[ ! -f "${scriptDir}/${relativeDir}/Makefile" && ! -f "${scriptDir}/${relativeDir}/makefile" ]]; then
    error "No se encontró Makefile en: ${relativeDir}"
    exit 1
  fi
}

buildTarget() {
  local relativeDir="$1"

  checkMakefile "${relativeDir}"

  info "Compilando ${relativeDir}..."
  make -C "${scriptDir}/${relativeDir}"
}

cleanTarget() {
  local relativeDir="$1"

  checkMakefile "${relativeDir}"

  info "Limpiando ${relativeDir}..."
  make -C "${scriptDir}/${relativeDir}" clean
}

verifyBinary() {
  local binaryPath="$1"

  if [[ ! -x "${scriptDir}/${binaryPath}" ]]; then
    error "No se encontró el ejecutable esperado: ${binaryPath}"
    exit 1
  fi
}

buildAll() {
  buildTarget "Serial/Compressor"
  buildTarget "Serial/Decompressor"
  buildTarget "Concurrent/Compressor"
  buildTarget "Concurrent/Decompressor"
  buildTarget "Parallel/Compressor"
  buildTarget "Parallel/Decompressor"
}

cleanAll() {
  cleanTarget "Serial/Compressor"
  cleanTarget "Serial/Decompressor"
  cleanTarget "Concurrent/Compressor"
  cleanTarget "Concurrent/Decompressor"
  cleanTarget "Parallel/Compressor"
  cleanTarget "Parallel/Decompressor"
}

verifyAll() {
  verifyBinary "Serial/Compressor/compress"
  verifyBinary "Serial/Decompressor/decompress"
  verifyBinary "Concurrent/Compressor/compress_pthread"
  verifyBinary "Concurrent/Decompressor/decompress_pthread"
  verifyBinary "Parallel/Compressor/compress_parallel"
  verifyBinary "Parallel/Decompressor/decompress_parallel"
}

printUsage() {
  printf 'Uso: %s [auto|build|clean|rebuild|help]\n' "$(basename "$0")"
}

printGeneratedExecutables() {
  printf 'Ejecutables generados:\n'
  printf '  - Serial/Compressor/compress\n'
  printf '  - Serial/Decompressor/decompress\n'
  printf '  - Concurrent/Compressor/compress_pthread\n'
  printf '  - Concurrent/Decompressor/decompress_pthread\n'
  printf '  - Parallel/Compressor/compress_parallel\n'
  printf '  - Parallel/Decompressor/decompress_parallel\n'
}

main() {
  requireLinuxDebian

  case "${mode}" in
    auto)
      configureAptCommand
      installDependencies
      cleanAll
      buildAll
      verifyAll
      ;;
    build)
      configureAptCommand
      installDependencies
      buildAll
      verifyAll
      ;;
    clean)
      cleanAll
      ;;
    rebuild)
      configureAptCommand
      installDependencies
      cleanAll
      buildAll
      verifyAll
      ;;
    help|-h|--help)
      printUsage
      exit 0
      ;;
    *)
      error "Modo inválido: ${mode}"
      printUsage
      exit 1
      ;;
  esac

  info "Proyecto listo."
  printf '\n'

  if [[ "${mode}" == "clean" ]]; then
    printf 'Se limpiaron los binarios generados por los makefiles.\n'
  else
    if [[ "${mode}" == "auto" ]]; then
      printf 'Se ejecutó limpieza y recompilación completa automáticamente.\n'
    fi

    printGeneratedExecutables
  fi
}

main "$@"
