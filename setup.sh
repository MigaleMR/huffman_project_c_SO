#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APT_CMD=()
MODE="${1:-auto}"

info() {
  printf '[INFO] %s\n' "$1"
}

error() {
  printf '[ERROR] %s\n' "$1" >&2
}

require_linux_debian() {
  if [[ "$(uname -s)" != "Linux" ]]; then
    error "Este script esta pensado para Linux."
    exit 1
  fi

  if [[ ! -f /etc/os-release ]]; then
    error "No se pudo detectar la distribucion."
    exit 1
  fi

  # shellcheck disable=SC1091
  source /etc/os-release

  if [[ "${ID:-}" != "debian" ]] && [[ "${ID_LIKE:-}" != *"debian"* ]]; then
    error "Este setup.sh esta hecho para Debian o derivadas compatibles con apt."
    exit 1
  fi

  if ! command -v apt-get >/dev/null 2>&1; then
    error "No se encontro apt-get en el sistema."
    exit 1
  fi
}

configure_apt_command() {
  if [[ "$(id -u)" -eq 0 ]]; then
    APT_CMD=(apt-get)
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    APT_CMD=(sudo apt-get)
    return
  fi

  error "Se necesita ejecutar como root o tener sudo disponible para instalar dependencias."
  exit 1
}

install_dependencies() {
  info "Actualizando indices de paquetes..."
  "${APT_CMD[@]}" update

  info "Instalando dependencias de compilacion..."
  "${APT_CMD[@]}" install -y build-essential
}

build_target() {
  local relative_dir="$1"

  info "Compilando ${relative_dir}..."
  make -C "${SCRIPT_DIR}/${relative_dir}"
}

clean_target() {
  local relative_dir="$1"

  info "Limpiando ${relative_dir}..."
  make -C "${SCRIPT_DIR}/${relative_dir}" clean
}

verify_binary() {
  local binary_path="$1"

  if [[ ! -x "${SCRIPT_DIR}/${binary_path}" ]]; then
    error "No se encontro el ejecutable esperado: ${binary_path}"
    exit 1
  fi
}

build_all() {
  build_target "Serial/Compressor"
  build_target "Serial/Decompressor"
  build_target "Concurrent/Compressor"
  build_target "Concurrent/Decompressor"
  build_target "Parallel/Compressor"
  build_target "Parallel/Decompressor"
}

clean_all() {
  clean_target "Serial/Compressor"
  clean_target "Serial/Decompressor"
  clean_target "Concurrent/Compressor"
  clean_target "Concurrent/Decompressor"
  clean_target "Parallel/Compressor"
  clean_target "Parallel/Decompressor"
}

verify_all() {
  verify_binary "Serial/Compressor/compress"
  verify_binary "Serial/Decompressor/decompress"
  verify_binary "Concurrent/Compressor/compress_pthread"
  verify_binary "Concurrent/Decompressor/decompress_pthread"
  verify_binary "Parallel/Compressor/compress_parallel"
  verify_binary "Parallel/Decompressor/decompress_parallel"
}

print_usage() {
  printf 'Uso: %s [auto|build|clean|rebuild|help]\n' "$(basename "$0")"
}

main() {
  require_linux_debian

  case "${MODE}" in
    auto)
      configure_apt_command
      install_dependencies
      clean_all
      build_all
      verify_all
      ;;
    build)
      configure_apt_command
      install_dependencies
      build_all
      verify_all
      ;;
    clean)
      clean_all
      ;;
    rebuild)
      configure_apt_command
      install_dependencies
      clean_all
      build_all
      verify_all
      ;;
    help|-h|--help)
      print_usage
      exit 0
      ;;
    *)
      error "Modo invalido: ${MODE}"
      print_usage
      exit 1
      ;;
  esac

  info "Proyecto listo."
  printf '\n'
  if [[ "${MODE}" == "clean" ]]; then
    printf 'Se limpiaron los binarios generados por los makefiles.\n'
  else
    if [[ "${MODE}" == "auto" ]]; then
      printf 'Se ejecuto limpieza y recompilacion completa automaticamente.\n'
    fi
      printf 'Ejecutables generados:\n'
      printf '  - Serial/Compressor/compress\n'
      printf '  - Serial/Decompressor/decompress\n'
      printf '  - Concurrent/Compressor/compress_pthread\n'
      printf '  - Concurrent/Decompressor/decompress_pthread\n'
      printf '  - Parallel/Compressor/compress_parallel\n'
      printf '  - Parallel/Decompressor/decompress_parallel\n'
  fi
}

main "$@"
