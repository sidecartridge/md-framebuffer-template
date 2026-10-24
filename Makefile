# Version from file
VERSION := $(shell cat version.txt)

# The app settings' layout version a release must carry: ACONFIG_VERSION_NUMBER
# in rp/src/include/aconfig.h. Stored settings of another version are ignored
# and the defaults load, so an app raises both together when a release must
# start its settings again; a development-only value never reaches a tag.
RELEASE_ACONFIG_VERSION ?= 1
ACONFIG_H ?= rp/src/include/aconfig.h
ACONFIG_VERSION := $(shell grep -oE 'ACONFIG_VERSION_NUMBER = [0-9A-Fa-fx]+' $(ACONFIG_H) | grep -oE '[0-9A-Fa-fx]+$$')

## Check that this tree can be released
.PHONY: check-release
check-release:
	@test "$$(printf '%d' '$(ACONFIG_VERSION)' 2>/dev/null)" = "$$(printf '%d' '$(RELEASE_ACONFIG_VERSION)')" || { \
	  echo "ERROR: ACONFIG_VERSION_NUMBER in $(ACONFIG_H) is '$(ACONFIG_VERSION)'; a release needs $(RELEASE_ACONFIG_VERSION)"; \
	  exit 1; }
	@echo "ACONFIG_VERSION_NUMBER is $(ACONFIG_VERSION)"

## Tag this version
.PHONY: tag
tag: check-release
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"
