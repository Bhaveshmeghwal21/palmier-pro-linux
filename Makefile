# SPDX-License-Identifier: GPL-3.0-or-later
#
# Palmier Pro for Linux — task runner.
#
# Every target here is the same command .github/workflows/ci.yml runs, so a green
# `make ci` locally means the same thing CI means. The configure flags in particular
# are copied from the workflow rather than paraphrased: if they drift, the vendor-path
# assertions in CI fail and say so.
#
# Quick start on a clean Ubuntu 24.04 machine:
#
#     make deps        # install the native toolchain (needs sudo)
#     make build       # configure + compile
#     make test        # run the whole suite headlessly
#
# Or with nothing but Docker installed:
#
#     make docker-test
#
# Run `make` or `make help` for the full list.

BUILD_DIR   ?= build
BUILD_TYPE  ?= RelWithDebInfo
GENERATOR   ?= Ninja
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)
DOCKER_IMAGE ?= palmier-pro:local

# Coverage is a compiler-flag concern rather than a project feature, so it is injected
# through the standard CMake flag variables instead of a bespoke PALMIER_* option.
# That matters here: cmake/PalmierOptionsManifest.cmake emits every PALMIER_* cache
# entry and the documentation checker fails until docs/BUILD.md documents each one, so
# a coverage option would couple "measure coverage" to an option-table edit for no gain.
COVERAGE_DIR   ?= build-coverage
COVERAGE_FLAGS ?= --coverage -fprofile-update=atomic -O0 -g

# The vendor hardware paths are configured ON deliberately, matching CI: it is what
# makes the VAAPI/NVENC/QSV code compile and type-check. Hardware *execution* still
# falls back to software on a host with no such device.
CMAKE_FLAGS ?= \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DPALMIER_BUILD_TESTS=ON \
	-DPALMIER_BUILD_UI=ON \
	-DPALMIER_ENABLE_VAAPI=ON \
	-DPALMIER_ENABLE_NVENC=ON \
	-DPALMIER_ENABLE_QSV=ON

# C++ sources this repository owns, used by the format and tidy targets.
CXX_SOURCES := $(shell find src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null)

# Files touched relative to origin/main — what the CI lint gate checks. Falls back to
# the working tree's staged/unstaged changes when there is no upstream to compare to.
CHANGED_CXX := $(shell git diff --name-only --diff-filter=ACMR origin/main...HEAD 2>/dev/null \
	| grep -E '^(src|tests)/.*\.(cpp|hpp)$$' || true)

.DEFAULT_GOAL := help
.PHONY: help deps configure build test test-verbose coverage coverage-report lint format \
        format-check tidy py-lint py-test run package clean distclean ci docker-build \
        docker-test docker-run print-changed

help: ## Show this help
	@printf 'Palmier Pro for Linux — available targets\n\n'
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| sort \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'
	@printf '\nVariables: BUILD_DIR=%s BUILD_TYPE=%s JOBS=%s\n' '$(BUILD_DIR)' '$(BUILD_TYPE)' '$(JOBS)'

deps: ## Install the native build dependencies (Ubuntu/Debian, needs sudo)
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends $$(sed -e 's/#.*//' packaging/apt-packages.txt)
	@printf '\nffnvcodec headers are not in every archive snapshot; if pkg-config cannot\n'
	@printf 'find ffnvcodec, see docs/BUILD.md for the upstream source install.\n'

configure: ## Configure the build tree with CMake
	cmake -S . -B $(BUILD_DIR) -G $(GENERATOR) $(CMAKE_FLAGS)

build: configure ## Configure and compile everything
	cmake --build $(BUILD_DIR) --parallel $(JOBS)

test: ## Run the full test suite headlessly (needs a built tree)
	xvfb-run -a ctest --test-dir $(BUILD_DIR) --output-on-failure

test-verbose: ## Run the suite with per-test output and a JUnit report
	set -o pipefail; xvfb-run -a ctest --test-dir $(BUILD_DIR) \
		--output-on-failure --output-junit ctest-results.xml 2>&1 | tee $(BUILD_DIR)/ctest.log
	python3 scripts/ctest_summary.py --junit $(BUILD_DIR)/ctest-results.xml || true

coverage: ## Build instrumented, run the suite, and report line coverage
	cmake -S . -B $(COVERAGE_DIR) -G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DPALMIER_BUILD_TESTS=ON \
		-DPALMIER_BUILD_UI=ON \
		-DCMAKE_CXX_FLAGS="$(COVERAGE_FLAGS)" \
		-DCMAKE_EXE_LINKER_FLAGS="$(COVERAGE_FLAGS)" \
		-DCMAKE_SHARED_LINKER_FLAGS="$(COVERAGE_FLAGS)"
	cmake --build $(COVERAGE_DIR) --parallel $(JOBS)
	xvfb-run -a ctest --test-dir $(COVERAGE_DIR) --output-on-failure
	$(MAKE) coverage-report

coverage-report: ## Turn the .gcda files already produced into coverage.info and a summary
	lcov --directory $(COVERAGE_DIR) --capture --output-file coverage.info \
		--rc branch_coverage=1 --ignore-errors mismatch,negative,source,gcov
	lcov --remove coverage.info \
		'/usr/*' '*/build*/*' '*/tests/*' '*/_deps/*' '*/moc_*' '*/qrc_*' '*/ui_*' \
		--output-file coverage.info --ignore-errors unused
	lcov --list coverage.info
	lcov --summary coverage.info

lint: format-check py-lint ## Run every non-blocking-free check the CI lint job runs

format: ## Reformat this repository's C++ sources in place
	@printf 'formatting %s files\n' '$(words $(CXX_SOURCES))'
	clang-format -i $(CXX_SOURCES)

format-check: ## Fail if the files changed against origin/main are not formatted
	@if [ -z "$(CHANGED_CXX)" ]; then \
		printf 'no C++ files changed against origin/main; nothing to check\n'; \
	else \
		printf 'checking formatting of:\n'; \
		for f in $(CHANGED_CXX); do printf '  %s\n' "$$f"; done; \
		clang-format --dry-run --Werror $(CHANGED_CXX); \
		printf 'formatting OK\n'; \
	fi

tidy: ## Run clang-tidy over the changed files (needs a configured build tree)
	@if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
		printf 'no %s/compile_commands.json; run `make configure` first\n' '$(BUILD_DIR)'; exit 1; \
	fi
	@if [ -z "$(CHANGED_CXX)" ]; then \
		printf 'no C++ files changed against origin/main; nothing to tidy\n'; \
	else \
		clang-tidy -p $(BUILD_DIR) $(filter %.cpp,$(CHANGED_CXX)); \
	fi

py-lint: ## Lint and type-check the Python CI tooling
	ruff check scripts
	ruff format --check scripts
	python3 -m mypy scripts || printf 'mypy reported findings (advisory)\n'

py-test: ## Run the Python CI tooling's own tests
	python3 -m unittest discover -s scripts -p 'test_*.py' -v

run: ## Launch the editor from the build tree
	$(BUILD_DIR)/bin/palmier-pro

package: ## Build the installable .deb with CPack
	cmake -S . -B $(BUILD_DIR)-package -G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DPALMIER_BUILD_TESTS=OFF -DPALMIER_BUILD_UI=ON
	cmake --build $(BUILD_DIR)-package --parallel $(JOBS)
	cd $(BUILD_DIR)-package && cpack -G DEB

ci: build test lint py-test ## Everything CI checks, in CI's order

docker-build: ## Build the container image with the whole toolchain baked in
	docker build -t $(DOCKER_IMAGE) .

docker-test: docker-build ## Configure, build and test inside the container
	docker run --rm $(DOCKER_IMAGE) make build test

docker-run: docker-build ## Launch the editor in the container on a virtual display
	docker run --rm $(DOCKER_IMAGE) \
		sh -c 'xvfb-run -a $(BUILD_DIR)/bin/palmier-pro --help || true'

print-changed: ## Show which C++ files the format gate would check
	@for f in $(CHANGED_CXX); do printf '%s\n' "$$f"; done

clean: ## Remove build artefacts
	rm -rf $(BUILD_DIR) $(COVERAGE_DIR) $(BUILD_DIR)-package

distclean: clean ## Remove build artefacts and generated reports
	rm -f configure.log ctest-results.xml coverage.info
	rm -rf coverage-html
