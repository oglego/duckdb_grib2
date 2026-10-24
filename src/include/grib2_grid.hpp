#pragma once

// Geometry helpers for GRIB2 grids.
//
// Deliberately free of DuckDB and ecCodes dependencies so they can be unit
// tested on their own (see test/cpp/grid_check.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace duckdb {

struct GridCell {
	int64_t i;
	int64_t j;
	double lat;
	double lon;
	double distance_km;
};

//! Wrap a longitude in degrees into [-180, 180). Accepts both the 0..360 and
//! -180..180 conventions.
inline double WrapLongitude180(double lon) {
	return lon - 360.0 * std::floor((lon + 180.0) / 360.0);
}

//! Index into ecCodes' "values" array for the cell (i, j), where i increases
//! east and j increases north.
//!
//! With alternative row scanning (scanning mode flag bit 4) adjacent rows are
//! stored in opposite directions, and ecCodes returns "values" in that stored
//! order while latitudes and longitudes are in canonical order. Pairing
//! values[k] with lat/lon[k] would silently mirror every other row. Assumes
//! iScansNegatively = 0, jScansPositively = 1 and jPointsAreConsecutive = 0;
//! callers must reject other scanning modes.
inline int64_t RawValueIndex(int64_t ni, int64_t i, int64_t j, bool alternative_row_scanning) {
	const bool reversed_row = alternative_row_scanning && (j & 1) != 0;
	return j * ni + (reversed_row ? ni - 1 - i : i);
}

//! Spherical Lambert conformal conic grid (GRIB2 grid template 3.30). Cell (i,
//! j) lives in row-major order with i increasing east and j increasing north.
class LambertGrid {
public:
	LambertGrid(int64_t ni_p, int64_t nj_p, double la1_deg, double lo1_deg, double lad_deg, double lov_deg,
	            double latin1_deg, double latin2_deg, double dx_m, double dy_m, double radius_m)
	    : ni(ni_p), nj(nj_p), dx(dx_m), dy(dy_m), radius(radius_m), lov(lov_deg) {
		const double p1 = latin1_deg * DEG;
		const double p2 = latin2_deg * DEG;
		const double p0 = lad_deg * DEG;
		if (std::fabs(p1 - p2) < 1e-12) {
			n = std::sin(p1);
		} else {
			n = std::log(std::cos(p1) / std::cos(p2)) / std::log(std::tan(PI / 4 + p2 / 2) / std::tan(PI / 4 + p1 / 2));
		}
		f = std::cos(p1) * std::pow(std::tan(PI / 4 + p1 / 2), n) / n;
		rho0 = radius * f / std::pow(std::tan(PI / 4 + p0 / 2), n);
		Project(la1_deg, lo1_deg, x1, y1);
	}

	int64_t Ni() const {
		return ni;
	}
	int64_t Nj() const {
		return nj;
	}

	//! Fractional grid coordinates (fi, fj) of a lat/lon point. Cell centres are
	//! at integer coordinates.
	void Forward(double lat_deg, double lon_deg, double &fi, double &fj) const {
		double x;
		double y;
		Project(lat_deg, lon_deg, x, y);
		fi = (x - x1) / dx;
		fj = (y - y1) / dy;
	}

	//! Latitude and longitude (longitude in -180..180) of the centre of cell (i,
	//! j).
	void Inverse(double i, double j, double &lat_deg, double &lon_deg) const {
		const double x = x1 + i * dx;
		const double y = y1 + j * dy;
		const double sign = n < 0 ? -1.0 : 1.0;
		const double dy_rho = rho0 - y;
		const double rho = sign * std::hypot(x, dy_rho);
		const double theta = std::atan2(sign * x, sign * dy_rho);
		lat_deg = (2 * std::atan(std::pow(radius * f / rho, 1.0 / n)) - PI / 2) / DEG;
		lon_deg = WrapLongitude180(lov + theta / n / DEG);
	}

	//! Nearest cell to a lat/lon point. Returns false when the point lies outside
	//! the grid footprint.
	bool NearestCell(double lat_deg, double lon_deg, GridCell &out) const {
		double fi;
		double fj;
		Forward(lat_deg, lon_deg, fi, fj);
		if (!std::isfinite(fi) || !std::isfinite(fj) || fi < -0.5 || fj < -0.5 || fi > ni - 0.5 || fj > nj - 0.5) {
			return false;
		}
		const int64_t ci = static_cast<int64_t>(std::floor(fi + 0.5));
		const int64_t cj = static_cast<int64_t>(std::floor(fj + 0.5));
		bool found = false;
		// The grid is regular in projected space, so the nearest cell on the sphere
		// is the rounded cell or one of its eight neighbours.
		for (int64_t dj = -1; dj <= 1; dj++) {
			for (int64_t di = -1; di <= 1; di++) {
				const int64_t i = ci + di;
				const int64_t j = cj + dj;
				if (i < 0 || j < 0 || i >= ni || j >= nj) {
					continue;
				}
				double cell_lat;
				double cell_lon;
				Inverse(static_cast<double>(i), static_cast<double>(j), cell_lat, cell_lon);
				const double distance = HaversineKm(lat_deg, lon_deg, cell_lat, cell_lon);
				if (!found || distance < out.distance_km) {
					out = GridCell {i, j, cell_lat, cell_lon, distance};
					found = true;
				}
			}
		}
		return found;
	}

	//! Appends the flat index (j * ni + i) of every cell whose centre lies inside
	//! the lat/lon box, in ascending order. Longitudes may use either convention.
	//! A box with lon_max < lon_min crosses the antimeridian.
	void CellsInBox(double lon_min, double lat_min, double lon_max, double lat_max, std::vector<int64_t> &out) const {
		// Width of the box in degrees east, computed before wrapping so both
		// longitude conventions work
		double span = lon_max - lon_min;
		if (span < 0) {
			span += 360.0;
		}
		span = std::min(span, 360.0);
		lon_min = WrapLongitude180(lon_min);

		// Project the box outline. The image of a simply connected region is
		// bounded by the image of its boundary, so the outline gives the grid
		// window to scan.
		const int samples = 64;
		double min_fi = INFINITY;
		double max_fi = -INFINITY;
		double min_fj = INFINITY;
		double max_fj = -INFINITY;
		bool finite = true;
		for (int s = 0; s < samples; s++) {
			const double t = static_cast<double>(s) / (samples - 1);
			const double lon = lon_min + span * t;
			const double lat = lat_min + (lat_max - lat_min) * t;
			const double edge_lat[4] = {lat_min, lat, lat_max, lat};
			const double edge_lon[4] = {lon, lon_min + span, lon, lon_min};
			for (int e = 0; e < 4; e++) {
				double fi;
				double fj;
				Forward(edge_lat[e], edge_lon[e], fi, fj);
				if (!std::isfinite(fi) || !std::isfinite(fj)) {
					finite = false;
					continue;
				}
				min_fi = std::min(min_fi, fi);
				max_fi = std::max(max_fi, fi);
				min_fj = std::min(min_fj, fj);
				max_fj = std::max(max_fj, fj);
			}
		}
		int64_t i0 = 0;
		int64_t i1 = ni - 1;
		int64_t j0 = 0;
		int64_t j1 = nj - 1;
		if (finite) {
			i0 = std::max<int64_t>(static_cast<int64_t>(std::floor(min_fi)) - 2, 0);
			i1 = std::min<int64_t>(static_cast<int64_t>(std::ceil(max_fi)) + 2, ni - 1);
			j0 = std::max<int64_t>(static_cast<int64_t>(std::floor(min_fj)) - 2, 0);
			j1 = std::min<int64_t>(static_cast<int64_t>(std::ceil(max_fj)) + 2, nj - 1);
		}
		for (int64_t j = j0; j <= j1; j++) {
			for (int64_t i = i0; i <= i1; i++) {
				double lat;
				double lon;
				Inverse(static_cast<double>(i), static_cast<double>(j), lat, lon);
				const double east_of_min = WrapLongitude180(lon - lon_min - 180.0) + 180.0;
				if (lat >= lat_min && lat <= lat_max && east_of_min <= span) {
					out.push_back(j * ni + i);
				}
			}
		}
	}

private:
	static constexpr double PI = 3.14159265358979323846;
	static constexpr double DEG = PI / 180.0;

	void Project(double lat_deg, double lon_deg, double &x, double &y) const {
		// Clamp so the poles do not divide by zero
		const double lat = std::max(-89.999999, std::min(89.999999, lat_deg));
		const double phi = lat * DEG;
		const double rho = radius * f / std::pow(std::tan(PI / 4 + phi / 2), n);
		const double theta = n * WrapLongitude180(lon_deg - lov) * DEG;
		x = rho * std::sin(theta);
		y = rho0 - rho * std::cos(theta);
	}

	double HaversineKm(double lat1, double lon1, double lat2, double lon2) const {
		const double p1 = lat1 * DEG;
		const double p2 = lat2 * DEG;
		const double dl = (lon2 - lon1) * DEG;
		const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) +
		                 std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
		return 2 * (radius / 1000.0) * std::asin(std::sqrt(a));
	}

	int64_t ni;
	int64_t nj;
	double dx;
	double dy;
	double radius;
	double lov;
	double n = 0;
	double f = 0;
	double rho0 = 0;
	double x1 = 0;
	double y1 = 0;
};

} // namespace duckdb
