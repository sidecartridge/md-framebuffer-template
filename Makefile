# Version from file
VERSION := $(shell cat version.txt)

## Tag this version
.PHONY: tag
tag:
	git tag $(VERSION) && git push origin $(VERSION) && \
	echo "Tagged: $(VERSION)"
