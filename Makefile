.PHONY: build build-map build-server build-web test run clean

build: build-map build-server build-web

build-map:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j4

build-server:
	mkdir -p .cache/go-build build
	cd apps/control-server && GOCACHE=$(CURDIR)/.cache/go-build go build -o ../../build/zeus-server .
	cd apps/control-server && GOCACHE=$(CURDIR)/.cache/go-build go build -o ../../build/zeus-osm-turns ./cmd/zeus-osm-turns

build-web:
	npm --prefix apps/web install
	npm --prefix apps/web run build

test: build-map
	ctest --test-dir build --output-on-failure
	cd apps/control-server && GOCACHE=$(CURDIR)/.cache/go-build go test ./...
	npm --prefix apps/web run typecheck

run: build
	./build/zeus-server --addr 127.0.0.1:8080 --data-dir data --zeus-map ./build/zeus-map --web-dir ./apps/web/dist

clean:
	cmake -E remove_directory build
	cmake -E remove_directory apps/web/dist
