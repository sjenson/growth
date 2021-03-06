// simulation.cu
// parallel implementation of cellular growth
// Sage Jenson, 2017

#include "simulation.h"

using point_t = std::array<double,3>;

Simulation::Simulation(){}

void Simulation::set_parameters(const Parameters& _param)
{
	param = _param;
}

void Simulation::init(Parameters _param)
{
	num_threads = std::max(uint(1), std::thread::hardware_concurrency() - 2);
    param = _param;
    cells.reserve(MAX_POP);
    frame_num = 0;

    Geometry g;
    cells = g.create_geometry(param);

    if (param.food_mode == Food::INHERIT)
    {
        for (auto& p : cells)
        {
			p->inherited += std::pow((double) std::rand() / RAND_MAX, 100.0);
        }
    }

    if (param.food_mode == Food::TENTACLE)
    {
        cells[0]->special = true;
        for (auto& p : cells)
        {
            p->generation = 99;
        }
    }
}


std::vector<Particle*>* Simulation::get_cells()
{
    return &cells;
}

Vec3* Simulation::get_pos(int i)
{
    return &(cells[i]->position);
}

int Simulation::get_pop()
{
    return cells.size();
}

void Simulation::set_matrices()
{
    V.resize(cells.size(), 3);
    N.resize(cells.size(), 3);
    for (const auto& p : cells)
    {
        V.row(p->index) << p->position.transpose();
        N.row(p->index) << p->normal.transpose();
    }

    int num_faces = 0;
    for (const auto& p : cells)
    {
        num_faces += p->links.size();
    }

    F.resize(num_faces, 3);
    size_t cur_face = 0;
    for (const auto& p : cells)
    {
        for (size_t i=0; i<p->links.size(); i++)
        {
            const int c_index = p->links[i]->index;
            const int b_index = p->links[(i+1)% (p->links.size())]->index;
            F.row(cur_face) << p->index, b_index, c_index;
            cur_face++;
        }
    }
}

void Simulation::update()
{
    if (cells.size() < MAX_POP)
    {
        std::cout << "Add Food. " << std::flush;
        add_food();

        std::cout << "Split. " << std::flush;
        split();
    }

	std::cout << "Collision. " << std::flush;
	collision_tree();

    std::cout << "CPU forces. "<< std::flush;
    add_cpu_forces();

    std::cout << "Update. " << std::flush;
    update_position();

    std::cout << "Frame: " << frame_num;

    std::cout <<" Pop: " << cells.size() << "." << std::endl;

    frame_num++;
}

void Simulation::freeze_bad_particles()
{
    for (auto& p : cells)
    {
        if (not (p->environs || p->frozen || p->good_loop()))
        {
            p->frozen = true;
        }
    }
}

void Simulation::update_position()
{
    frozen_num = 0;
    for (auto& p : cells)
    {
        if (p->frozen)
        {
            frozen_num++;
        } else
        {
            p->update(param.dampening);
        }
    }
}

void Simulation::parallel_cpu_forces(size_t min, size_t max)
{

	for (size_t idx = min; idx < max; ++idx)
	{
		Particle* p = cells[idx];

        if (param.init_shape == Shape::ENVIRONMENT)
        {
            if (p->environs || p->frozen)
            {
                continue;
            }
        }

        if (!p->frozen)
        {
            p->calculate(param.spring_factor, param.planar_factor,
                    param.bulge_factor, param.spring_length);
        }
	}
}

void Simulation::add_cpu_forces()
{
	std::vector<std::thread> threads;

	for (size_t i = 0; i < num_threads; ++i)
	{
		size_t low = i * cells.size() / num_threads;
		size_t high = (i + 1) * cells.size() / num_threads;
		threads.emplace_back(std::thread(&Simulation::parallel_cpu_forces, this, low, high));
	}

	for (auto& thread : threads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}
}

void Simulation::add_food()
{
    for (auto& p : cells)
    {
        if (p->environs || p->frozen)
        {
            p->food = 0;
            continue;
        } else
        {
            if (param.food_mode == Food::RANDOM)
            {
                p->food += (float) std::rand() / RAND_MAX;
            }
			else if (param.food_mode == Food::AREA)
            {
                p->food += p->area;
            }
			else if (param.food_mode == Food::X_COORD)
            {
                p->food += p->position.x() + 50;
            }
			else if (param.food_mode == Food::RADIAL)
            {
                float dist = p->position.norm();
                if (dist < 0.5)
                {
                    dist = 0.5;
                }
                dist = pow(dist, 2);
                p->food += 100.0/dist;
            }
			else if (param.food_mode == Food::COLLISIONS)
            {
                if (p->collisions > 0)
                {
					p->food += 1.0 / p->collisions;
                }
            }
			else if (param.food_mode == Food::CURVATURE)
            {
                p->calculate_curvature();
                double amount = p->curvature;
				/*
                for (Particle* q : p->links)
                {
                    q->calculate_curvature();
                    amount += q->curvature;
                }
				*/
                if ((!std::isnan(amount) && (amount > 0)))
                {
                    p->food += std::pow(amount, param.curvature_factor);
                }
            }
			else if (param.food_mode == Food::INHERIT)
            {
                p->food += p->inherited;
            }
			else if (param.food_mode == Food::HYBRID)
            {
                p->calculate_curvature();
                float amount = p->curvature;
                if ((!std::isnan(amount)) && (amount > 0))
                {
                    p->food += amount * p->area;
                }
            }
			else if (param.food_mode == Food::SHIFT)
            {
                if (frame_num < 250)
                {
                    p->food += p->area;
                }
				else
                {
                    p->calculate_curvature();
                    float amount = p->curvature;
                    if ((!std::isnan(amount) && (amount > 0)))
                    {
                        p->food += amount;
                    }
                }
            }
			else if (param.food_mode == Food::TENTACLE)
            {
                if (p->special)
                {
                    p->food += p->area;
                    if (frame_num % 1500 == 1499)
                    {
                        p->special_baby = true;
                    }
                }
				else if (p->generation < 2)
                {
                    p->food += p->area;
                }
            }
        }
    }
}

void Simulation::split()
{
    bool did_split = false;
    int new_pop = cells.size();
    int fixed_pop = new_pop;
    for (auto i=0; i<fixed_pop; i++)
    {
        if (cells[i]->frozen || cells[i]->environs)
        {
            continue;
        }
        if (cells[i]->food > param.threshold || cells[i]->get_num_links()> param.max_degree)
        {
            if (new_pop == MAX_POP)
            {
                std::cout << "Maximum population of " <<
                    MAX_POP << " reached." << std::flush;
                return;
            }

            if (!cells[i]->good_loop())
            {
                cells[i]->frozen = true;
                continue;
            }

            if (!did_split)
            {
                std::cout << "Did split. " << std::flush;
                did_split = true;
            }

            cells.push_back(new Particle(new_pop));

            if (param.split_mode == Split::ZERO)
            {
                cells[i]->split(cells.back(), false);
            } else if (param.split_mode == Split::LONG)
            {
                cells[i]->split(cells.back(), true);
            }

            if (!cells[new_pop]->good_loop())
            {
                cells[new_pop]->frozen = true;
            }

            new_pop++;
        }
    }
}


void Simulation::collision_update_cell_range(size_t min, size_t max, const tree::KDTree<Particle*, 3>& tree, size_t max_neighbors)
{
    const double c_sq = param.collision_radius * param.collision_radius;
	for (size_t idx = min; idx < max; ++idx)
	{
		Particle* p = cells[idx];
        //if (p->age > param.collision_age_threshold) continue;

        for (auto n : tree.searchCapacityLimitedBall(point_t{{p->position.x(),p->position.y(),p->position.z()}}, c_sq, max_neighbors))
		{
			auto& q = n.payload;
            Vec3 disp = p->position - q->position;

            float dist = disp.squaredNorm();

            if ((p!=q) and (!p->connected_to(q)))
            {
                disp.normalize();
                disp *= (c_sq - dist) / c_sq;
                p->collision_target += disp;
                p->collisions++;
            }
        }
	}
}

void Simulation::collision_tree()
{
	// build tree
	using tree_t = tree::KDTree<Particle*, 3>;

	tree_t tree;

	for (const auto& p: cells)
	{
		tree.addPoint(point_t{{p->position.x(),p->position.y(),p->position.z()}}, p, false);
	}
	tree.splitOutstanding();

	const size_t max_neighbors{10};

	std::vector<std::thread> threads;

	for (size_t i = 0; i < num_threads; ++i)
	{
		size_t low = i * cells.size() / num_threads;
		size_t high = (i + 1) * cells.size() / num_threads;
		threads.emplace_back(std::thread(&Simulation::collision_update_cell_range, this, low, high, std::cref(tree), max_neighbors));
	}

	for (auto& thread : threads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}

    for (auto& p : cells)
    {
        if (p->collisions != 0)
        {
            p->collision_target /= (float) p->collisions;
            p->collision_target *= param.collision_factor;
            p->delta = p->collision_target;
        }
    }
}

void Simulation::collision_grid()
{
    Grid* _g = make_grid();
    std::unique_ptr<Grid> g(_g);
    int sorted[cells.size()];
    Box boxes[g->box_num];

    g->create_box_list(sorted, boxes);
    for (auto& p : cells)
    {
        g->set_box(p);
    }

    float c_sq = param.collision_radius * param.collision_radius;
    for (auto& p : cells)
	{
        if (p->age > param.collision_age_threshold) continue;
        for (auto n : g->get_neighbors(p))
		{
            auto& q = cells[n];
            Vec3 disp = p->position - q->position;
            float dist = disp.squaredNorm();
            if ((p!=q) and (dist < c_sq) and (!p->connected_to(q)))
            {
                float dist = disp.squaredNorm();
                disp.normalize();
                disp *= (c_sq - dist) / c_sq;
                p->collision_target += disp;
                p->collisions++;
            }
        }
    }

    for (auto& p : cells)
    {
        if (p->collisions != 0)
        {
            p->collision_target /= (float) p->collisions;
            p->collision_target *= param.collision_factor;
            p->delta = p->collision_target;
        }
    }
}

void Simulation::brute_force_collision(){
    float c_sq = param.collision_radius * param.collision_radius;
    for (size_t i=0; i<cells.size(); i++)
    {
        for (size_t j=0; j<cells.size(); j++)
        {
            Vec3 disp = cells[i]->position - cells[j]->position;
            float dist = disp.squaredNorm();
            if ((i!=j) and (dist < c_sq) and (!cells[i]->connected_to(cells[j])))
            {
                float dist = disp.squaredNorm();
                disp.normalize();
                disp *= (c_sq - dist) / c_sq;
                cells[i]->collision_target += disp;
                cells[i]->collisions++;
            }
        }
    }

    for (auto& p : cells)
    {
        if (p->collisions != 0)
        {
            p->collision_target /= (float) p->collisions;
            p->collision_target *= param.collision_factor;
            p->delta = p->collision_target;
        }
    }
}

Grid* Simulation::make_grid()
{
    Grid* g;
    int big = std::numeric_limits<int>::max();
    float max_x(-big), max_y(-big), max_z(-big);
    float min_x(big), min_y(big), min_z(big);

    for (auto& p : cells)
    {
        Vec3 pos = p->position;
        if (max_x < pos.x()) max_x = pos.x();
        if (max_y < pos.y()) max_y = pos.y();
        if (max_z < pos.z()) max_z = pos.z();
        if (min_x > pos.x()) min_x = pos.x();
        if (min_y > pos.y()) min_y = pos.y();
        if (min_z > pos.z()) min_z = pos.z();
    }

    Vec3 mx(max_x, max_y, max_z);
    Vec3 mn(min_x, min_y, min_z);

    g = new Grid(mx, mn, param.collision_radius);

    for (auto& p : cells)
    {
        g->add_point(p->position, p->index);
    }
    return g;
}

Simulation::~Simulation(){
   for (auto it = cells.begin(); it != cells.end(); ++it){
       delete *it;
   }
}
