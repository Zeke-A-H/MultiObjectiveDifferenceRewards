#include "evolutionary.h"
#include "environment.h"
#include "policy.h"
#include "team.h"
#include <vector>
#include <yaml-cpp/yaml.h>
#include <pagmo/utils/hypervolume.hpp>
#include <pagmo/types.hpp>
#include <limits>

const int NONE = std::numeric_limits<int>::min();

Individual::Individual(const std::string& filename, int id) : team(filename, id), id(id) {
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
    fitness = combinedCumulativeRewards;
    return combinedCumulativeRewards;
}

// compute and update the difference evaluations member variable
void Individual::differenceEvaluate(const std::string& filename, std::vector<Environment> environments, std::vector<Individual> paretoFront, int paretoIndex, double hypervolume, double lowerBound) {
    YAML::Node config = YAML::LoadFile(filename);
    const YAML::Node& evolutionary_config = config["evolutionary"];
    std::string counterfactualType = evolutionary_config["counterfactualType"].as<std::string>();
    
    EvolutionaryUtils evoHelper;
    
    // 1. get and sum the replay rewards for each environment in environments
    std::vector<std::vector<int>> agentWiseCumulativeReplayRewards;
    for (auto environ : environments) {
        if (agentWiseCumulativeReplayRewards.size() == 0) {
            agentWiseCumulativeReplayRewards = team.replayWithCounterfactual(filename, environ, counterfactualType);
        } else {
            std::vector<std::vector<int>> replayRewards = team.replayWithCounterfactual(filename, environ, counterfactualType);
            for (int counterfactualNumber = 0; counterfactualNumber < agentWiseCumulativeReplayRewards.size(); counterfactualNumber++) { // for all agents in the team
                for (int rewNumber = 0; rewNumber < agentWiseCumulativeReplayRewards[counterfactualNumber].size(); rewNumber++) { // add up the counterfactual rewards
                    agentWiseCumulativeReplayRewards[counterfactualNumber][rewNumber] += replayRewards[counterfactualNumber][rewNumber];
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

    for (int i=0; i<this->team.agents.size(); i++) { // add each counterfactual fitness to the working pareto front
        paretoFitnesses.push_back(agentWiseCumulativeReplayRewards[i]);
        double counterfactualHypervolume = evoHelper.getHypervolume(paretoFitnesses, lowerBound); // get the hypervolume with this counterfactual fitness inserted
        double differenceImpact = hypervolume - counterfactualHypervolume; // find the difference with actual pareto hypervolume
        this->differenceEvaluations.push_back (differenceImpact); // assign the difference impact to the agent
        paretoFitnesses.pop_back();// delete the last (counterfactual) fitness from the pareto fitnesses
    }
}

Evolutionary::Evolutionary(const std::string& filename) {
    YAML::Node config = YAML::LoadFile(filename);
    const YAML::Node& evolutionary_config = config["evolutionary"];

    numberOfGenerations = evolutionary_config["numberOfGenerations"].as<int>();
    numberOfEpisodes = evolutionary_config["numberOfEpisodes"].as<int>();
    populationSize = evolutionary_config["populationSize"].as<int>();
    teamIDCounter = 0;

    for (int i=0; i < populationSize; i++) {
        population.push_back(Individual(filename, teamIDCounter++)); // Create a population of individuals with id
    }
}

// DECIDE and Generate as many environment configurations as numberOfEpisodes
std::vector<Environment> Evolutionary::generateTestEnvironments
    (const std::string& filename) {
    YAML::Node config = YAML::LoadFile(filename);

    int numberOfEnvironments = numberOfEpisodes;
    bool differentEnvs = config["environment"]["differentEnvs"].as<bool>();

    std::vector<Environment> testEnvironments;
    
    Environment env;
    env.loadConfig(filename);
    for(int i = 0; i < numberOfEnvironments; i++) {
        // Load up a new env configuration if env should be different for each episode
        if (differentEnvs) {
            env.reset();
            env.loadConfig(filename);
        }
        testEnvironments.push_back(env);
    }

    std::cout<<"''''''''''''''\nGenerated "<<numberOfEnvironments<<" environments.";
    for (auto env : testEnvironments) {
        std::cout<<"\nEnv:\n";
        env.printInfo();
    }
    std::cout<<"'''''''''''''\n";

    return testEnvironments;
}

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


// Actually run the simulation across teams and evolve them
void Evolutionary::evolve(const std::string& filename) {
    std::vector<Environment> envs = generateTestEnvironments(filename);

    EvolutionaryUtils evoHelper;

    // Compute the origin for the hypervolume computation
    YAML::Node config = YAML::LoadFile(filename);
    const int lowerBound = config["team"]["numberOfAgents"].as<int>()
                                    * config["episode"]["length"].as<int>()
                                    * config["environment"]["penalty"].as<int>()
                                    * config["evolutionary"]["numberOfEpisodes"].as<int>() - 1;
    
    // How many offsprings does the generation create?
    const int numberOfOffsprings = config["evolutionary"]["numberOfOffsprings"].as<int>();
    
    std::cout<<"Hypervolume origin is: "<<lowerBound<<std::endl;

    for (auto &ind : population) {
        ind.evaluate(filename, envs);
    }

    std::vector<std::vector<Individual>> paretoFronts; // Better PFs first

    // 1. Get at least 'numberOfOffsprings' solutions from the population into as many pareto fronts as needed
    std::vector<Individual> workingPopulation = this->population; // temporary population variable to generate the pareto fronts

    while (workingPopulation.size() > this->population.size() - numberOfOffsprings) {
        paretoFronts.push_back(evoHelper.findParetoFront(workingPopulation));
         workingPopulation = evoHelper.without(workingPopulation, paretoFronts.back()); // remove the newest pareto front from working population
    }

    // 2. Update agent-level difference impact/reward for each solution on the above pareto fronts
    paretoFronts[0][0].differenceEvaluate(filename, envs, paretoFronts[0], 0, evoHelper.getHypervolume(paretoFronts[0], lowerBound), lowerBound);
    int vv;
    
}
