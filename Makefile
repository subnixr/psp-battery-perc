IMAGE ?= pspdev/pspdev:latest

FW_URL = https://archive.org/download/psp_ofw_firmwares
FW     = fw/661.PBP fw/661go.PBP

DOCKER_RUN = docker run --rm \
	-v $(CURDIR):/work -w /work/src \
	-u $(shell id -u):$(shell id -g) \
	$(IMAGE)

.PHONY: all clean shell pull fw

all:
	$(DOCKER_RUN) make $(if $(DEBUG),DEBUG=1) $(if $(BATLOG),BATLOG=1)

clean:
	$(DOCKER_RUN) make clean

shell:
	docker run --rm -it \
		-v $(CURDIR):/work -w /work/src \
		-u $(shell id -u):$(shell id -g) \
		$(IMAGE) sh

pull:
	docker pull $(IMAGE)

fw: $(FW)

fw/661.PBP:
	mkdir -p fw
	curl -fL -o $@.part $(FW_URL)/PSP/661.PBP
	mv $@.part $@

fw/661go.PBP:
	mkdir -p fw
	curl -fL -o $@.part "$(FW_URL)/PSP%20Go/661go.PBP"
	mv $@.part $@
