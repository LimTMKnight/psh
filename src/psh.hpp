#pragma once

#include <string>
#include <functional>
#include <cmath>
#include <random>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>
#include "tbb/parallel_sort.h"
#include "tbb/mutex.h"
#include "tbb/pipeline.h"
#include "util.hpp"
#include "point.hpp"

#define VALUE(x) std::cout << #x "=" << x << std::endl

namespace psh
{
	template<uint d, class T>
	class map
	{
		static_assert(d > 0, "d must be larger than 0.");
	public:
		struct data_t
		{
			point<d> location;
			T contents;
		};

		struct bucket : public std::vector<data_t>
		{
			uint phi_index;

			bucket(uint phi_index) : phi_index(phi_index) { }
			friend bool operator<(const bucket& lhs, const bucket& rhs) {
				return lhs.size() > rhs.size();
			}
		};

		struct entry
		{
			uint k;
			uint hk;
			T contents;

			entry() : k(1), hk(1), contents(T()) { };
			entry(const data_t& data, uint M2) : k(1), contents(data.contents)
			{
				rehash(data, M2);
			}

			static constexpr uint h(const point<d>& p, uint M2, uint k)
			{
				return p * point<d>::increasing_pow(k) * M2;
			}

			void rehash(const point<d>& location, uint M2, uint new_k = 1)
			{
				k = new_k;
				hk = h(location, M2, k);
			}

			void rehash(const data_t& data, uint M2, uint new_k = 1)
			{
				rehash(data.location, M2, new_k);
			}

			constexpr bool equals(const point<d>& p, uint M2) const
			{
				return hk == h(p, M2, k);
			}
		};

		struct entry_large : public entry
		{
			point<d> location;
			entry_large() : entry(), location(point<d>()) { };
			entry_large(const data_t& data, uint M2)
				: entry(data, M2), location(data.location) { };
			void rehash(uint M2) { entry::rehash(location, M2, entry::k + 1); };
		};

		uint M0;
		uint M1;
		uint M2;
		uint n;
		uint m_bar;
		uint m;
		uint r_bar;
		uint r;
		std::vector<point<d>> phi;
		std::vector<entry> H;
		std::default_random_engine generator;

		map(const std::vector<data_t>& data, const point<d>& domain_size)
			: n(data.size()), m_bar(std::ceil(std::pow(n, 1.0f / d))), m(std::pow(m_bar, d)),
			  r_bar(std::ceil(std::pow(n / d, 1.0f / d)) - 1), generator(time(0))
		{
			M0 = prime();
			while ((M1 = prime()) == M0);
			M2 = prime();

			VALUE(m);
			VALUE(m_bar);

			VALUE(M0);
			VALUE(M1);
			VALUE(M2);

			bool create_succeeded = false;

			std::uniform_int_distribution<uint> m_dist(0, m - 1);

			do
			{
				r_bar += d;
				r = std::pow(r_bar, d);
				VALUE(r);
				VALUE(r_bar);

				create_succeeded = create(data, domain_size, m_dist);

			} while (!create_succeeded);
		}

		uint prime()
		{
			static const std::vector<uint> primes{ 53, 97, 193, 389, 769, 1543, 3079,
				6151, 12289, 24593, 49157, 98317, 196613, 393241, 786433, 1572869,
				3145739, 6291469 };
			static std::uniform_int_distribution<uint> prime_dist(0, primes.size() - 1);

			return primes[prime_dist(generator)];
		}

		bool bad_m_r()
		{
			auto m_mod_r = m_bar % r_bar;
			return m_mod_r == 1 || m_mod_r == r - 1;
		}

		void insert(const bucket& b, std::vector<entry_large>& H_hat, std::vector<bool>& H_b_hat,
			const decltype(phi)& phi_hat)
		{
			for (auto& element : b)
			{
				auto hashed = h(element.location, phi_hat);
				auto i = point_to_index<d>(hashed, m_bar, m);
				H_hat[i] = entry_large(element, M2);
				H_b_hat[i] = true;
			}
		}

		bool jiggle_offsets(std::vector<entry_large>& H_hat, std::vector<bool>& H_b_hat,
			decltype(phi)& phi_hat, const bucket& b,
			std::uniform_int_distribution<uint>& m_dist)
		{
			uint start_offset = m_dist(generator);

			bool found = false;
			point<d> found_offset;
			tbb::mutex mutex;

			uint chunk_index = 0;
			const uint num_cores = 8;
			const uint group_size = r / num_cores + 1;

			tbb::parallel_pipeline(num_cores,
				tbb::make_filter<void, uint>(tbb::filter::serial,
					[=, &chunk_index, &found, &group_size](tbb::flow_control& fc) {
						if (found || chunk_index >= r)
						{
							fc.stop();
						}
						chunk_index += group_size;
						return chunk_index;
					}) &
				tbb::make_filter<uint, void>(tbb::filter::parallel,
					[=, &mutex, &found, &found_offset, &b, &phi_hat, &H_hat, &H_b_hat](uint i0)
					{
						for (uint i = i0; i < i0 + group_size && !found; i++)
						{
							auto phi_offset = index_to_point<d>((start_offset + i) % m, m_bar, m);

							bool collision = false;
							for (auto& element : b)
							{
								auto h0 = M0 * element.location;
								auto h1 = M1 * element.location;
								auto index = point_to_index<d>(h1, r_bar, r);
								auto offset = index == b.phi_index ? phi_offset : phi_hat[index];
								auto hash = h0 + offset;

								collision = H_b_hat[point_to_index<d>(hash, m_bar, m)];
								if (collision)
									break;
							}

							if (!collision)
							{
								tbb::mutex::scoped_lock lock(mutex);
								if (!found)
								{
									found = true;
									found_offset = phi_offset;
								}
							}
						}
					})
				);
			if (found)
			{
				phi_hat[b.phi_index] = found_offset;
				insert(b, H_hat, H_b_hat, phi_hat);
				return true;
			}
			return false;
		}

		std::vector<bucket> create_buckets(const std::vector<data_t>& data)
		{
			std::vector<bucket> buckets;
			buckets.reserve(r);
			{
				uint i = 0;
				std::generate_n(std::back_inserter(buckets), r, [&] {
						return bucket(i++);
					});
			}

			for (auto& element : data)
			{
				auto h1 = M1 * element.location;
				buckets[point_to_index<d>(h1, r_bar, r)].push_back(element);
			}

			std::cout << "buckets created" << std::endl;

			tbb::parallel_sort(buckets.begin(), buckets.end());
			std::cout << "buckets sorted" << std::endl;

			return buckets;
		}

		bool create(const std::vector<data_t>& data, const point<d>& domain_size,
			std::uniform_int_distribution<uint>& m_dist)
		{
			decltype(phi) phi_hat(r);
			std::vector<entry_large> H_hat(m);
			std::vector<bool> H_b_hat(m, false);
			std::cout << "creating " << r << " buckets" << std::endl;

			if (bad_m_r())
				return false;

			auto buckets = create_buckets(data);
			std::cout << "jiggling offsets" << std::endl;

			for (uint i = 0; i < buckets.size(); i++)
			{
				if (buckets[i].size() == 0)
					break;
				if (i % (buckets.size() / 10) == 0)
					std::cout << (100 * i) / buckets.size() << "% done" << std::endl;

				if (!jiggle_offsets(H_hat, H_b_hat, phi_hat, buckets[i], m_dist))
				{
					return false;
				}
			}

			std::cout << "done!" << std::endl;
			phi = std::move(phi_hat);
			hash_positions(data, domain_size, H_hat);
			std::copy(H_hat.begin(), H_hat.end(), std::back_inserter(H));

			return true;
		}

		void hash_positions(const std::vector<data_t>& data, const point<d>& domain_size,
			std::vector<entry_large>& H_hat)
		{
			// domain_size - 1 to get the highest indices in each direction
			// width is assumed to be equal in all directions
			// we don't expect it to roll over so we mod by the maximum uint, -1
			// and then add 1 to get the size, not the index
			uint domain_i_max = point_to_index<d>(domain_size - 1, domain_size[0],
				uint(-1)) + 1;
			std::vector<bool> indices(m);
			{
				uint j = 0;
				uint k = point_to_index<d>(data[j].location, domain_size[0], uint(-1));

				// first sweep
				for (uint i = 0; i < domain_i_max; i++)
				{
					if (k == i)
					{
						j++;
						k = point_to_index<d>(data[j].location, domain_size[0], uint(-1));
						continue;
					}
					// for each point p in original image which is empty

					auto p = index_to_point<d>(i, domain_size[0], uint(-1));
					uint l = point_to_index<d>(h(p), m_bar, m);
					if (H_hat[l].hk == entry::h(p, M2, 1))
					{
						//     if (get(p).hk == p.hk)
						//         indices.add(p)
						indices[l] = true;
					}
				}
				std::cout << "data size: " << data.size() << std::endl;
				std::cout << "indices size: " << indices.size() << std::endl;
			}

			std::unordered_map<uint, std::vector<uint>> collisions;
			{
				// second sweep
				for (uint i = 0; i < domain_i_max; i++)
				{
					// for each point p in original image

					auto p = index_to_point<d>(i, domain_size[0], uint(-1));
					uint l = point_to_index<d>(h(p), m_bar, m);
					// it's like collect everyone that maps to the same thing
					if (indices[l])
					{
						collisions[l].push_back(i);
					}
				}
			}

			// third sweep
			for (auto& kvp : collisions)
			{
			rehash:
				// kvp.first == l == location in H
				H_hat[kvp.first].rehash(M2);

				for (uint i : kvp.second)
				{
					// i == index in U, does not exist in input data
					// if one of these have the same hk as som H_hat[kvp.first]
					// break, success = false, call the po-po
					auto p = index_to_point<d>(i, domain_size[0], uint(-1));
					uint hk = entry::h(p, M2, H_hat[kvp.first].k);
					if (H_hat[kvp.first].location != p && H_hat[kvp.first].hk == hk)
					{
						goto rehash;
					}
				}
			}
		}

		point<d> h(const point<d>& p, const decltype(phi)& phi_hat) const
		{
			auto h0 = M0 * p;
			auto h1 = M1 * p;
			auto i = point_to_index<d>(h1, r_bar, r);
			auto offset = phi_hat[i];
			return h0 + offset;
		}

		point<d> h(const point<d>& p) const
		{
			return h(p, phi);
		}

		T get(const point<d>& p) const
		{
			auto i = point_to_index<d>(h(p), m_bar, m);
			if (H[i].equals(p, M2))
				return H[i].contents;
			else
				throw std::out_of_range("Element not found in map");
		}

		uint memory_size() const
		{
			return sizeof(*this)
				+ sizeof(typename decltype(phi)::value_type) * phi.capacity()
				+ sizeof(typename decltype(H)::value_type) * H.capacity();
		}
	};
}