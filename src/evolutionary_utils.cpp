#include "evolutionary_utils.h"
#include "environment.h"
#include "policy.h"
#include "team.h"
#include <yaml-cpp/yaml.h>
#include <pagmo/utils/hypervolume.hpp>
#include <pagmo/types.hpp>

const int NONE = std::numeric_limits<int>::min();

// constructor
EvolutionaryUtils::EvolutionaryUtils() {
    x=2;
}

// Compute the hypervolume contained by the given pareto front
double EvolutionaryUtils::getHypervolume(std::vector<Individual> individuals, int lowerBound) {
    // get the hypervolume computation reference point from the origin
    // reference poitn is -ve of original as pagmo likes it to be bigger than any other point
    // but for us it is smaller, so ive flipped signs everywhere for hypervolume computattion
    pagmo::vector_double referencePoint(individuals[0].fitness.size(), -lowerBound);
   
    // Just a dirty way to get the fitnesses from the individuals and feed to pagmo hypervol compute
    std::vector<pagmo::vector_double> fitnesses;
    for (auto ind : individuals) {
        pagmo::vector_double fit;
        for (auto f:ind.fitness)
            fit.push_back(-f);
        fitnesses.push_back(fit);
    }
    pagmo::hypervolume h(fitnesses);
    return h.compute(referencePoint);

}

// Compute the hypervolume contained by the given pareto front
double EvolutionaryUtils::getHypervolume(std::vector<std::vector<int>> individualFitnesses, int lowerBound) {
    // get the hypervolume computation reference point from the origin
    // reference poitn is -ve of original as pagmo likes it to be bigger than any other point
    // but for us it is smaller, so ive flipped signs everywhere for hypervolume computattion
    pagmo::vector_double referencePoint(individualFitnesses[0].size(), -lowerBound);
   
    // Just a dirty way to get the fitnesses from the individuals and feed to pagmo hypervol compute
    std::vector<pagmo::vector_double> fitnesses;
    for (auto fitness : individualFitnesses) {
        pagmo::vector_double fit;
        for (auto f:fitness)
            fit.push_back(-f);
        fitnesses.push_back(fit);
    }
    pagmo::hypervolume h(fitnesses);
    return h.compute(referencePoint);

}

// finds if the individual a dominates individual b
bool EvolutionaryUtils::dominates(Individual a, Individual b) {
    if (a.fitness.size() != b.fitness.size()) {
        std::cout<<"Cannot find dominating solution. Imbalanced fitnesses";
        exit(1);
    }
    else if (a.fitness[0] == NONE || b.fitness[0] == NONE) {
        std::cout<<"Cannot find dominating solution. NONE fitnesses";
        exit(1);
    }

    for (int i = 0; i < a.fitness.size(); i++) {
        if (a.fitness[i] <= b.fitness[i])
            return false;
    }

    return true;
}

// Find and return the pareto front of the given population
std::vector<Individual> EvolutionaryUtils::findParetoFront(const std::vector<Individual>& population) {
    std::vector<Individual> paretoFront;

    for (const Individual& individual : population) {
        bool isNonDominated = true;

        // Check if the individual is non-dominated by comparing its fitness with others
        for (const Individual& other : population) {
            if (&individual != &other) { // Skip self-comparison
            // if other dominates individual, then individual should not be on pareto front
                if (dominates(other, individual)) {
                    isNonDominated = false;
                    break;
                }
            }
        }

        // If the individual is non-dominated, add it to the Pareto front
        if (isNonDominated) {
            paretoFront.push_back(individual);
        }
    }

    return paretoFront;
}

// Return a population without the provided solutions
std::vector<Individual> EvolutionaryUtils::without(const std::vector<Individual> workingPopulation, const std::vector<Individual> toRemoveSolutions) {
    std::vector<Individual> populationWithout;

    // Search for a population-member in the to-remove solutions
    // If not found, then add it to the 'without' population
    for (auto ind : workingPopulation) {
        bool found = false;
        for (auto sol : toRemoveSolutions) {
            if (sol.id == ind.id) {
                found = true;
                break;
            }
        }
        if (!found) {
            populationWithout.push_back(ind);
        }
    }

    return populationWithout;
}

std::vector<Individual> EvolutionaryUtils::cull(const std::vector<Individual> PF, const int desiredSize) {
    std::vector<Individual> culledPF;
    
    // group individuals according to their fitnesses
    std::vector<std::vector<Individual>> groupedIndividuals;
    for (const auto& ind : PF) {
        bool found = false;

        // Iterate over existing groups
        for (auto& group : groupedIndividuals) {
            // Check if the fitness vectors match
            if (group.front().fitness == ind.fitness) {
                group.push_back(ind);
                found = true;
                break;
            }
        }
        // If no matching group found, create a new group
        if (!found) {
            groupedIndividuals.push_back({ind});
        }
    }

    // add unique fitness individuals to culledPF
    while (true) {
        for (auto &group : groupedIndividuals) {
            if (group.size() > 0) {
                culledPF.push_back(group.back());
                group.pop_back();
            }

            if (culledPF.size() >= desiredSize)
                return culledPF;
        }
    }
}

// Select an element from a row using softmax selection
int EvolutionaryUtils::softmaxSelection(std::vector<double> probabilities) {
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 gen(rd()); // Seed the random number engine with rd
    // Define a distribution for the random numbers
    std::uniform_int_distribution<int> randomIndexDistribution(0, probabilities.size()-1); // Generate integers between 0 and length of proabbilities
    
    // find the min element
    int minElement = *(std::min_element(probabilities.begin(), probabilities.end()));
    if (minElement < 0) {
        // adjust all values
        for (int i=0; i<probabilities.size(); i++) {
            probabilities[i] -= minElement;
        }
    }

    // find the max element
    int maxElement = *(std::max_element(probabilities.begin(), probabilities.end()));
    // return a random index if the max element is 0 (ie all elems are 0)
    if (maxElement <= 0) {
        return randomIndexDistribution(gen);
    }

    // normalise the probabilities values
    double sumOfAllElements = 0;
    for (int i=0; i<probabilities.size(); i++) {
        sumOfAllElements += probabilities[i];
    }
    for (int i=0; i<probabilities.size(); i++) {
        probabilities[i] /= sumOfAllElements;
    }

    std::uniform_real_distribution<double> randomProbabilityDistribution(0, 1);
    double selectionProbability = randomProbabilityDistribution(gen);

    // roulette wheel selection on the normalised probabilities values
    double cumulativeProbability = 0.0;
    
    for (int i=0; i<probabilities.size(); i++) {
        cumulativeProbability += probabilities[i];
        if (selectionProbability <= cumulativeProbability) {
            return i;
        }
    }

    for (auto x: probabilities) {
        std::cout<<x<<" ";
    }

    std::cout<<"Softmax failed";
    exit(1);
}

// return the transpose of a mtrix
std::vector<std::vector<double>> EvolutionaryUtils::transpose(std::vector<std::vector<double>> matrix) {
    std::vector<std::vector<double>> t_amtrix(matrix[0].size(), std::vector<double>(matrix.size()));
    // Transpose the matrix
    for (size_t i = 0; i < matrix.size(); ++i) {
        for (size_t j = 0; j < matrix[i].size(); ++j) {
            t_amtrix[j][i] = matrix[i][j];
        }
    }

    return t_amtrix;
}

// retyurn a column from the matrix
std::vector<double> EvolutionaryUtils::getColumn(std::vector<std::vector<double>> matrix, int colNum) {
    std::vector<double> column;
    // Transpose the matrix
    for (size_t i = 0; i < matrix.size(); ++i) {
        column.push_back(matrix[i][colNum]);
    }

    return column;
}

Individual::Individual(const std::string& filename, int id) : team(filename, id), id(id) {
    YAML::Node config = YAML::LoadFile(filename);

    // Initialise the fitness of the individual as NONE
    int numberOfObjectives = config["environment"]["numberOfClassIds"].as<int>();
    for(int i = 0; i < numberOfObjectives; i++) {
        fitness.push_back(NONE);
    }
}

Individual::Individual(const std::string& filename, int id, std::vector<Agent> agents) : team(filename, agents, id), id(id) {
    YAML::Node config = YAML::LoadFile(filename);

    // Initialise the fitness of the individual as NONE
    int numberOfObjectives = config["environment"]["numberOfClassIds"].as<int>();
    for(int i = 0; i < numberOfObjectives; i++) {
        fitness.push_back(NONE);
    }
}

std::vector<int> Individual::evaluate(const std::string& filename, std::vector<Environment> environments) {
    std::vector<std::vector<int>> stepwiseEpisodeReward; // Reward vector from each step of an episode
    std::vector<int> cumulativeEpisodeReward; // Sum of stewise rewards of an episode
    std::vector<std::vector<int>> cumulativeRewardsFromEachEpisode; // List of the cumulative episode rewards
    std::vector<int> combinedCumulativeRewards; // Sum of the cumulative rewards
    
    for (Environment env : environments) {
        env.reset();
        env.loadConfig(filename);
        stepwiseEpisodeReward = team.simulate(filename, env);
        cumulativeEpisodeReward = std::vector<int>(stepwiseEpisodeReward[0].size(), 0);

        for (auto& episodeReward : stepwiseEpisodeReward) {
            for (size_t i=0; i< episodeReward.size(); i++) {
                cumulativeEpisodeReward[i] += episodeReward[i];
            }
        }
        // tag the episode reward at the end of lsit
        cumulativeRewardsFromEachEpisode.push_back(cumulativeEpisodeReward);
    }

    combinedCumulativeRewards = std::vector<int>(cumulativeRewardsFromEachEpisode[0].size(), 0);
    for (auto& cumulativeReward : cumulativeRewardsFromEachEpisode) {
        for (size_t i=0; i< cumulativeReward.size(); i++) {
            combinedCumulativeRewards[i] += cumulativeReward[i];
        }
    }

    // set the fitness of the individual
    this->fitness = combinedCumulativeRewards;
    return combinedCumulativeRewards;
}

// compute and update the difference evaluations member variable
void Individual::differenceEvaluate(const std::string& filename, std::vector<Environment> environments, std::vector<Individual> paretoFront, int paretoIndex, double hypervolume, double lowerBound) {
    YAML::Node config = YAML::LoadFile(filename);
    const YAML::Node& evolutionary_config = config["evolutionary"];
    std::string counterfactualType = evolutionary_config["counterfactualType"].as<std::string>();
    
    EvolutionaryUtils evoHelper;
    
    // 1. get and sum the replay rewards for each environment in environments
    std::vector<std::vector<int>> cumulativeReplayRewards;
    for (auto environ : environments) {
        if (cumulativeReplayRewards.size() == 0) {
            cumulativeReplayRewards = team.replayWithCounterfactual(filename, environ, counterfactualType);
        } else {
            std::vector<std::vector<int>> replayRewards = team.replayWithCounterfactual(filename, environ, counterfactualType);
            for (int i = 0; i < cumulativeReplayRewards.size(); i++) { // for all agents in the team
                for (int rewNumber = 0; rewNumber < cumulativeReplayRewards[i].size(); rewNumber++) { // add up the counterfactual rewards
                    cumulativeReplayRewards[i][rewNumber] += replayRewards[i][rewNumber];
                }
            }
        }
    }

    // 2. Get pareto front hypervolume with these rewards swapped in for the original agent rewards
    std::vector<std::vector<int>> paretoFitnesses; // temporary front to deal with new hypervolume computations for each agent
    for (int i=0; i<paretoFront.size(); i++) { // populate working pareto front with all but this individual
        if (i == paretoIndex) continue;
        else {
            paretoFitnesses.push_back(paretoFront[i].fitness);
        }
    }

    // if (team.agents.size() != 10) {
    //     std::cout<<"Invalid team size";
    //     exit(1);
    // }

    for (int i=0; i<this->team.agents.size(); i++) { // add each counterfactual fitness to the working pareto front
        paretoFitnesses.push_back(cumulativeReplayRewards[i]);
        double counterfactualHypervolume = evoHelper.getHypervolume(paretoFitnesses, lowerBound); // get the hypervolume with this counterfactual fitness inserted
        double differenceImpact = hypervolume - counterfactualHypervolume; // find the difference with actual pareto hypervolume
        this->differenceEvaluations.push_back (differenceImpact); // assign the difference impact to the agent
        paretoFitnesses.pop_back();// delete the last (counterfactual) fitness from the pareto fitnesses
    }
    // std::cout<<team.agents.size()<<std::endl;
}

// mutate the agents' policies with some noise
void Individual::mutate() {
    this->team.mutate();
}

// Return the team's agents
std::vector<Agent> Individual::getAgents() {
    return this->team.agents;
}

// return the individual's team trajectory
std::string Individual::getTeamTrajectoryAsString() {
    std::stringstream output;
    
    // return the trajectory transpose (so each row is one agent's trajectory now)
    for (int i=0; i<this->team.teamTrajectory[0].size(); i++) {
        for (int j=0; j < team.teamTrajectory.size(); j++) {
            output << team.teamTrajectory[j][i].first << "," << team.teamTrajectory[j][i].second << " ";
        }
        output << std::endl;
    }

    return output.str();
}
