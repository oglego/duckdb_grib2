// Standalone check of src/include/grib2_grid.hpp against ecCodes-derived
// reference data.
//
// Build and run from the repository root:
//   g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o
//   /tmp/grid_check && /tmp/grid_check
//
// The reference file is produced by test/data/generate_grid_reference.py.

#include "grib2_grid.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using namespace duckdb;

static int failures = 0;

static void Check(bool ok, const std::string &what) {
	if (!ok) {
		failures++;
		if (failures <= 20) {
			std::cerr << "FAIL: " << what << "\n";
		}
	}
}

int main(int argc, char **argv) {
	const std::string path = argc > 1 ? argv[1] : "test/data/grid_reference.txt";
	std::ifstream in(path);
	if (!in) {
		std::cerr << "cannot open " << path << " (run from the repository root)\n";
		return 2;
	}

	std::unique_ptr<LambertGrid> grid;
	bool alternative_rows = false;
	int cells = 0;
	int points = 0;
	int boxes = 0;
	double max_latlon_error = 0;
	double max_index_error = 0;

	std::string line;
	while (std::getline(in, line)) {
		std::istringstream row(line);
		std::string kind;
		row >> kind;
		if (kind == "grid") {
			int64_t ni;
			int64_t nj;
			double la1, lo1, lad, lov, latin1, latin2, dx, dy, radius;
			int alt;
			row >> ni >> nj >> la1 >> lo1 >> lad >> lov >> latin1 >> latin2 >> dx >> dy >> radius >> alt;
			grid = std::make_unique<LambertGrid>(ni, nj, la1, lo1, lad, lov, latin1, latin2, dx, dy, radius);
			alternative_rows = alt != 0;
		} else if (!grid) {
			std::cerr << "reference file must start with a grid line\n";
			return 2;
		} else if (kind == "cell") {
			int64_t i, j, raw_index;
			double lat, lon;
			row >> i >> j >> lat >> lon >> raw_index;
			double got_lat, got_lon, fi, fj;
			grid->Inverse(static_cast<double>(i), static_cast<double>(j), got_lat, got_lon);
			grid->Forward(lat, lon, fi, fj);
			max_latlon_error = std::max(max_latlon_error, std::fabs(got_lat - lat));
			max_latlon_error = std::max(max_latlon_error, std::fabs(WrapLongitude180(got_lon - lon)));
			max_index_error = std::max(max_index_error, std::max(std::fabs(fi - i), std::fabs(fj - j)));
			Check(RawValueIndex(grid->Ni(), i, j, alternative_rows) == raw_index,
			      "raw value index for cell " + std::to_string(i) + "," + std::to_string(j));
			cells++;
		} else if (kind == "point") {
			double qlat, qlon, distance;
			int64_t i, j;
			row >> qlat >> qlon >> i >> j >> distance;
			GridCell cell;
			const bool found = grid->NearestCell(qlat, qlon, cell);
			if (i < 0) {
				Check(!found, "point outside the grid should have no nearest cell");
			} else {
				Check(found && cell.i == i && cell.j == j,
				      "nearest cell for point " + std::to_string(qlat) + "," + std::to_string(qlon));
				Check(found && std::fabs(cell.distance_km - distance) < 1e-6, "nearest cell distance");
			}
			points++;
		} else if (kind == "box") {
			double lon_min, lat_min, lon_max, lat_max;
			int64_t expected;
			row >> lon_min >> lat_min >> lon_max >> lat_max >> expected;
			std::vector<int64_t> found;
			grid->CellsInBox(lon_min, lat_min, lon_max, lat_max, found);
			Check(static_cast<int64_t>(found.size()) == expected,
			      "box " + std::to_string(lon_min) + "," + std::to_string(lat_min) + "," + std::to_string(lon_max) +
			          "," + std::to_string(lat_max) + ": expected " + std::to_string(expected) + " cells, got " +
			          std::to_string(found.size()));
			Check(std::is_sorted(found.begin(), found.end()), "box cells are in ascending order");
			boxes++;
		}
	}

	Check(max_latlon_error < 1e-9, "inverse projection error " + std::to_string(max_latlon_error) + " degrees");
	Check(max_index_error < 1e-6, "forward projection error " + std::to_string(max_index_error) + " cells");

	std::printf("cells=%d points=%d boxes=%d max_latlon_error=%.3g deg "
	            "max_index_error=%.3g cells failures=%d\n",
	            cells, points, boxes, max_latlon_error, max_index_error, failures);
	return failures == 0 ? 0 : 1;
}
