package main

import (
	"crypto/sha256"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFirmwareProtocol(t *testing.T) {
	filename := filepath.Join(t.TempDir(), "firmware.bin")
	handler := firmwareHandler(filename)
	publish := func(data string) {
		t.Helper()
		if err := os.WriteFile(filename+".tmp", []byte(data), 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.Rename(filename+".tmp", filename); err != nil {
			t.Fatal(err)
		}
	}
	request := func(method, path, condition string, status int) *httptest.ResponseRecorder {
		t.Helper()
		r := httptest.NewRequest(method, path, nil)
		if condition != "" {
			r.Header.Set("If-Match", condition)
		}
		w := httptest.NewRecorder()
		handler.ServeHTTP(w, r)
		if w.Code != status {
			t.Fatalf("%s %s: got %d, want %d: %s", method, path, w.Code, status, w.Body.String())
		}
		if w.Header().Get("Cache-Control") != "no-store" {
			t.Fatal("response must prevent stale caches")
		}
		return w
	}
	request("OPTIONS", "/firmware", "", 503)
	request("GET", "/firmware", "", 503)
	request("GET", "/other", "", 404)
	for _, method := range []string{"POST", "PUT", "HEAD", "DELETE"} {
		if got := request(method, "/firmware", "", 405).Header().Get("Allow"); got != "OPTIONS, GET" {
			t.Fatalf("unexpected Allow: %s", got)
		}
	}
	for _, data := range []string{"", strings.Repeat("x", maxFirmwareSize+1)} {
		publish(data)
		request("GET", "/firmware", "", 503)
	}

	var previous string
	for _, data := range []string{"firmware A", "firmware B", "firmware A"} {
		publish(data)
		metadata := request("OPTIONS", "/firmware", "", 204)
		etag := fmt.Sprintf(`"%x"`, sha256.Sum256([]byte(data)))
		if metadata.Header().Get("ETag") != etag || metadata.Body.Len() != 0 {
			t.Fatal("OPTIONS must return only the current ETag")
		}
		if previous != "" {
			request("GET", "/firmware", previous, 412)
		}
		for _, condition := range []string{"", etag, "*", `"other", ` + etag} {
			download := request("GET", "/firmware", condition, 200)
			if download.Body.String() != data || download.Header().Get("ETag") != etag ||
				download.Header().Get("Content-Length") != fmt.Sprint(len(data)) ||
				download.Header().Get("Content-Type") != "application/octet-stream" {
				t.Fatal("download bytes and metadata differ")
			}
		}
		request("GET", "/firmware", "W/"+etag, 412)
		previous = etag
	}

	// Replace the publication precisely when response writing starts. The
	// response must still contain the snapshot whose ETag was just calculated.
	result := httptest.NewRecorder()
	writer := &replaceOnWrite{ResponseRecorder: result, replace: func() { publish("firmware C") }}
	handler.ServeHTTP(writer, httptest.NewRequest("GET", "/firmware", nil))
	if result.Body.String() != "firmware A" || result.Header().Get("ETag") != previous {
		t.Fatal("publication during GET changed the response snapshot")
	}
	if request("GET", "/firmware", "", 200).Body.String() != "firmware C" {
		t.Fatal("next request did not see the new publication")
	}
}

type replaceOnWrite struct {
	*httptest.ResponseRecorder
	replace func()
}

func (w *replaceOnWrite) Write(data []byte) (int, error) {
	w.replace()
	return w.ResponseRecorder.Write(data)
}
