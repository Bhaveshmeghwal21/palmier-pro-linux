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

# bash, not the default /bin/sh. On Ubuntu /bin/sh is dash, which has no
# `set -o pipefail`, and the test target depends on it: without pipefail a failing
# ctest piped into tee would report tee's exit status and a red suite would look green.
SHELL := /bin/bash

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
# No -g. gcov maps counters to lines through the .gcno files the compiler writes beside
# each object, not through DWARF, so debug info adds nothing to a coverage run except
# size — and with 92 test executables linked unoptimised, size is the constraint that
# actually bites on a CI runner. -fprofile-update=atomic because the suite runs tests
# in parallel and non-atomic counters lose increments.
COVERAGE_FLAGS ?= --coverage -fprofile-update=atomic -O0 -g0

# Minimum line coverage `coverage-report` will accept.
#
# 78 because the first instrumented run measured 80.1% (15570 of 19443 lines), so this
# is just under the real figure: close enough that a genuine regression trips it, with
# enough headroom that ordinary variation in which conditional paths a run takes does
# not. Raise it when the measured figure rises; do not raise it above what a run has
# actually reported.
#
# Line coverage only. Branch coverage measured 45.5%, which is a much weaker number and
# is deliberately NOT gated: a large share of the unhit branches are the vendor
# hardware-codec and Vulkan paths that no CI runner can execute, so gating branches
# would be gating on the absence of a GPU rather than on the quality of the tests.
COVERAGE_MIN_LINE ?= 78

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
.PHONY: help deps configure build test test-summary test-verbose coverage coverage-build \
        coverage-test coverage-report lint format format-check tidy py-lint py-test run \
        package clean distclean ci docker-build docker-test docker-run print-changed \
        check-consistency

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

test: ## Run the full test suite headlessly, with a JUnit report (needs a built tree)
	set -o pipefail; xvfb-run -a ctest --test-dir $(BUILD_DIR) \
		--output-on-failure \
		--output-junit ctest-results.xml 2>&1 | tee $(BUILD_DIR)/ctest.log

test-summary: ## Render the Markdown summary from the JUnit report `make test` wrote
	python3 scripts/ctest_summary.py $(BUILD_DIR)/ctest-results.xml \
		--title "CTest summary" --allow-missing

test-verbose: test test-summary ## Run the suite and render the summary

coverage: coverage-build coverage-test coverage-report ## Instrumented build, suite run, and a line-coverage report

coverage-build: ## Configure and compile an instrumented build tree
	cmake -S . -B $(COVERAGE_DIR) -G $(GENERATOR) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DPALMIER_BUILD_TESTS=ON \
		-DPALMIER_BUILD_UI=ON \
		-DCMAKE_CXX_FLAGS="$(COVERAGE_FLAGS)" \
		-DCMAKE_CXX_FLAGS_DEBUG="-g0" \
		-DCMAKE_EXE_LINKER_FLAGS="$(COVERAGE_FLAGS)" \
		-DCMAKE_SHARED_LINKER_FLAGS="$(COVERAGE_FLAGS)"
	cmake --build $(COVERAGE_DIR) --parallel $(JOBS)
	@printf '\ninstrumented tree size: '; du -sh $(COVERAGE_DIR) 2>/dev/null || true

coverage-test: ## Run the suite against the instrumented tree, producing .gcda files
	set -o pipefail; xvfb-run -a ctest --test-dir $(COVERAGE_DIR) \
		--output-on-failure 2>&1 | tee $(COVERAGE_DIR)/ctest.log

coverage-report: ## Turn the .gcda files into a report and enforce the floor
	@mkdir -p coverage-html
	@# --merge-mode-functions is load-bearing, not decoration. src/media/MediaProbe.cpp
	@# defines isFfmpegAvailable() twice — once under #ifdef PALMIER_HAVE_FFMPEG and
	@# once in the #else — so the same symbol legitimately has two line numbers. gcovr's
	@# default function-merge mode is `strict`, which treats that as an AssertionError
	@# and aborts the whole report. merge-use-line-min accepts it and takes the lower
	@# line. This affects only function-level aggregation; line coverage is unchanged.
	gcovr --root . \
		--filter 'src/' \
		--exclude '.*/tests/.*' \
		--gcov-ignore-parse-errors \
		--exclude-unreachable-branches \
		--merge-mode-functions=merge-use-line-min \
		--print-summary \
		--xml coverage.xml \
		--html-details coverage-html/index.html \
		--fail-under-line $(COVERAGE_MIN_LINE) \
		$(COVERAGE_DIR)
	@# lcov-format output for anything that expects coverage.info. Non-fatal because
	@# --lcov arrived in gcovr 7.0 and the report above is the actual gate.
	@gcovr --root . --filter 'src/' --exclude '.*/tests/.*' \
		--gcov-ignore-parse-errors --merge-mode-functions=merge-use-line-min \
		--lcov coverage.info $(COVERAGE_DIR) \
		|| printf 'note: this gcovr has no --lcov; coverage.xml and the HTML remain\n'

lint: format-check py-lint check-consistency ## Every check the CI lint job runs

check-consistency: ## Check the repository's cross-file claims agree with the code
	python3 scripts/check_repo_consistency.py

format: ## Reformat this repository's C++ sources in place
	@printf 'formatting %s files\n' '$(words $(CXX_SOURCES))'
	clang-format -i $(CXX_SOURCES)

format-check: ## Fail if the lines changed against origin/main are not formatted
	@if [ -z "$(CHANGED_CXX)" ]; then \
		printf 'no C++ files changed against origin/main; nothing to check\n'; \
	else \
		status=0; \
		for f in $(CHANGED_CXX); do \
			args=$$(git diff -U0 --diff-filter=ACMR origin/main...HEAD -- "$$f" \
			        | sed -n 's/^@@ -[0-9,]* +\([0-9][0-9]*\),\{0,1\}\([0-9]*\) @@.*/\1 \2/p' \
			        | awk '{ c = ($$2 == "" ? 1 : $$2); if (c > 0) printf " --lines=%d:%d", $$1, $$1 + c - 1 }'); \
			if [ -z "$$args" ]; then \
				printf '  ---- %s (no added or modified lines)\n' "$$f"; \
			elif clang-format $$args "$$f" | diff -q - "$$f" >/dev/null 2>&1; then \
				printf '  OK   %s\n' "$$f"; \
			else \
				status=1; \
				printf '  FAIL %s — the lines you changed are not formatted:\n' "$$f"; \
				clang-format $$args "$$f" | diff -u "$$f" - | sed 's/^/    /' | head -40; \
			fi; \
		done; \
		if [ "$$status" -ne 0 ]; then \
			printf "run 'make format' or clang-format -i on the lines you changed\n"; \
			exit 1; \
		fi; \
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
