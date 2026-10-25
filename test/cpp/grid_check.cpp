// Standalone check of src/include/grib2_grid.hpp against ecCodes-derived reference data for every NBM region.
//
// Build and run from the repository root:
//   g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
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
static std::string region = "?";

static void Check(bool ok, const std::string &what) {
	if (!ok) {
		failures++;
		if (failures <= 20) {
			std::cerr << "FAIL [" << region << "]: " << what << "\n";
		}
	}
}

struct RegionStats {
	int cells = 0;
	int points = 0;
	int boxes = 0;
	double max_latlon_error = 0;
	double max_index_error = 0;
};

static void Summarize(const std::string &name, const RegionStats &stats) {
	std::printf("%-3s cells=%d points=%d boxes=%d max_latlon_error=%.3g deg max_index_error=%.3g cells\n", name.c_str(),
	            stats.cells, stats.points, stats.boxes, stats.max_latlon_error, stats.max_index_error);
	Check(stats.max_latlon_error < 1e-9, "inverse projection error " + std::to_string(stats.max_latlon_error) + " deg");
	Check(stats.max_index_error < 1e-6, "forward projection error " + std::to_string(stats.max_index_error) + " cells");
}

int main(int argc, char **argv) {
	const std::string path = argc > 1 ? argv[1] : "test/data/grid_reference.txt";
	std::ifstream in(path);
	if (!in) {
		std::cerr << "cannot open " << path << " (run from the repository root)\n";
		return 2;
	}

	std::unique_ptr<ProjectedGrid> grid;
	bool alternative_rows = false;
	RegionStats stats;
	int regions = 0;

	std::string line;
	while (std::getline(in, line)) {
		std::istringstream row(line);
		std::string kind;
		row >> kind;
		if (kind == "region") {
			if (grid) {
				Summarize(region, stats);
			}
			row >> region;
			stats = RegionStats();
			grid.reset();
			regions++;
		} else if (kind == "grid") {
			std::string type;
			int64_t ni, nj;
			double la1, lo1, lad;
			row >> type >> ni >> nj >> la1 >> lo1 >> lad;
			int alt = 0;
			if (type == "lambert") {
				double lov, latin1, latin2, dx, dy, radius;
				row >> lov >> latin1 >> latin2 >> dx >> dy >> radius >> alt;
				grid = std::make_unique<LambertGrid>(ni, nj, la1, lo1, lad, lov, latin1, latin2, dx, dy, radius);
			} else if (type == "polar_stereographic") {
				double lov, dx, dy, radius;
				row >> lov >> dx >> dy >> radius >> alt;
				grid = std::make_unique<PolarStereographicGrid>(ni, nj, la1, lo1, lad, lov, dx, dy, radius);
			} else if (type == "mercator") {
				double dx, dy, radius;
				row >> dx >> dy >> radius >> alt;
				grid = std::make_unique<MercatorGrid>(ni, nj, la1, lo1, lad, dx, dy, radius);
			} else {
				std::cerr << "unknown grid type " << type << "\n";
				return 2;
			}
			alternative_rows = alt != 0;
		} else if (kind.empty() || kind[0] == '#') {
			continue;
		} else if (!grid) {
			std::cerr << "reference data must start each region with a grid line\n";
			return 2;
		} else if (kind == "cell") {
			int64_t i, j, raw_index;
			double lat, lon;
			row >> i >> j >> lat >> lon >> raw_index;
			double got_lat, got_lon, fi, fj;
			grid->Inverse(static_cast<double>(i), static_cast<double>(j), got_lat, got_lon);
			grid->Forward(lat, lon, fi, fj);
			stats.max_latlon_error = std::max(stats.max_latlon_error, std::fabs(got_lat - lat));
			stats.max_latlon_error = std::max(stats.max_latlon_error, std::fabs(WrapLongitude180(got_lon - lon)));
			stats.max_index_error = std::max(stats.max_index_error, std::max(std::fabs(fi - i), std::fabs(fj - j)));
			Check(RawValueIndex(grid->Ni(), i, j, alternative_rows) == raw_index,
			      "raw value index for cell " + std::to_string(i) + "," + std::to_string(j));
			stats.cells++;
		} else if (kind == "point") {
			double qlat, qlon, distance;
			int64_t i, j;
			row >> qlat >> qlon >> i >> j >> distance;
			GridCell cell;
			const bool found = grid->NearestCell(qlat, qlon, cell);
			if (i < 0) {
				Check(!found, "point " + std::to_string(qlat) + "," + std::to_string(qlon) +
				                  " is outside the grid and should have no nearest cell");
			} else {
				Check(found && cell.i == i && cell.j == j,
				      "nearest cell for point " + std::to_string(qlat) + "," + std::to_string(qlon));
				Check(found && std::fabs(cell.distance_km - distance) < 1e-6, "nearest cell distance");
			}
			stats.points++;
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
			stats.boxes++;
		}
	}
	if (grid) {
		Summarize(region, stats);
	}

	std::printf("regions=%d failures=%d\n", regions, failures);
	return failures == 0 && regions > 0 ? 0 : 1;
}
