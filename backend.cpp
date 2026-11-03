#include "httplib.h"
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <climits>
#include <sstream>
#include <algorithm>
#include <mutex>

using namespace std;

struct Flight {
    string flightID;
    int emergencyScore; // 1-10 (10 = Critical Fuel / Medical)
    int fuelTimeLeft;    // Minutes
    int delayMinutes;
    int passengers;

    double calculatePriority() const {
        if (emergencyScore > 5) return -10000.0 * emergencyScore;
        return (fuelTimeLeft * 2.0) - (delayMinutes * 1.5) - (passengers * 0.05);
    }
};

struct FlightComparator {
    bool operator()(const Flight& a, const Flight& b) {
        return a.calculatePriority() > b.calculatePriority();
    }
};

void heapify(vector<Flight>& arr, int n, int i) {
    int largest = i;
    int left = 2 * i + 1;
    int right = 2 * i + 2;

    if (left < n && arr[left].calculatePriority() > arr[largest].calculatePriority())
        largest = left;

    if (right < n && arr[right].calculatePriority() > arr[largest].calculatePriority())
        largest = right;

    if (largest != i) {
        swap(arr[i], arr[largest]);
        heapify(arr, n, largest);
    }
}

void heapSortFlights(vector<Flight>& arr) {
    int n = arr.size();
    for (int i = n / 2 - 1; i >= 0; i--)
        heapify(arr, n, i);
    for (int i = n - 1; i >= 0; i--) {
        swap(arr[0], arr[i]);
        heapify(arr, i, 0);
    }
}

struct Edge {
    int to;
    int weight;
    bool isOpen;
};

class AirportGraph {
public:
    int numNodes;
    vector<vector<Edge>> adj;
    vector<string> names;

    AirportGraph(int n = 5) : numNodes(n) {
        adj.resize(n);
        names = {"Runway-A", "Taxiway-Alpha", "Taxiway-Beta", "Gate-10", "Gate-12"};
    }

    void addEdge(int u, int v, int w) {
        adj[u].push_back({v, w, true});
        adj[v].push_back({u, w, true});
    }

    void setStatus(int u, int v, bool status) {
        for (auto& e : adj[u]) if (e.to == v) e.isOpen = status;
        for (auto& e : adj[v]) if (e.to == u) e.isOpen = status;
    }

    pair<int, vector<int>> dijkstra(int startNode, int endNode) {
        vector<int> dist(numNodes, INT_MAX);
        vector<int> parent(numNodes, -1);
        priority_queue<pair<int, int>, vector<pair<int, int>>, greater<pair<int, int>>> pq;

        dist[startNode] = 0;
        pq.push({0, startNode});

        while (!pq.empty()) {
            int u = pq.top().second;
            int d = pq.top().first;
            pq.pop();

            if (d > dist[u]) continue;

            for (const auto& e : adj[u]) {
                if (!e.isOpen) continue;
                int v = e.to;
                if (dist[u] + e.weight < dist[v]) {
                    dist[v] = dist[u] + e.weight;
                    parent[v] = u;
                    pq.push({dist[v], v});
                }
            }
        }

        vector<int> path;
        if (dist[endNode] == INT_MAX) return {-1, path};

        for (int at = endNode; at != -1; at = parent[at]) path.push_back(at);
        reverse(path.begin(), path.end());
        return {dist[endNode], path};
    }
};

unordered_map<string, Flight> flightRegistry; // CO-3 Hash Map
priority_queue<Flight, vector<Flight>, FlightComparator> runwayQueue; // CO-2 Priority Queue
AirportGraph airport(5);
mutex stateMutex;

void resetDefaultAirfield() {
    airport = AirportGraph(5);
    airport.addEdge(0, 1, 5);  // Runway-A <-> Alpha
    airport.addEdge(1, 2, 3);  // Alpha <-> Beta
    airport.addEdge(1, 3, 10); // Alpha <-> Gate-10
    airport.addEdge(2, 4, 4);  // Beta <-> Gate-12
    airport.addEdge(3, 4, 2);  // Gate-10 <-> Gate-12
}

string extractJsonField(const string& body, const string& key) {
    size_t pos = body.find("\"" + key + "\"");
    if (pos == string::npos) return "";
    size_t colon = body.find(":", pos);
    size_t start = body.find_first_not_of(" \t\n\r\"", colon + 1);
    size_t end = body.find_first_of(",}\" \t\n\r", start);
    return body.substr(start, end - start);
}

int main() {
    resetDefaultAirfield();

    // Default Flights
    vector<Flight> initial = {
        {"AI-202", 1, 45, 10, 180},
        {"BA-109", 1, 25, 30, 240},
        {"EK-501", 1, 60, 5,  310},
        {"EMERGENCY-99", 10, 12, 0, 150}
    };

    for (const auto& f : initial) {
        flightRegistry[f.flightID] = f;
        runwayQueue.push(f);
    }

    httplib::Server svr;

    // CORS Middleware
    svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

    // OPTIONS preflight
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
    });

    // API: Get Current Operational State
    svr.Get("/api/state", [](const httplib::Request&, httplib::Response& res) {
        lock_guard<mutex> lock(stateMutex);

        stringstream ss;
        ss << "{ \"queue\": [";

        auto tempQ = runwayQueue;
        bool first = true;
        while (!tempQ.empty()) {
            Flight f = tempQ.top();
            tempQ.pop();
            if (!first) ss << ",";
            ss << "{\"id\":\"" << f.flightID << "\","
               << "\"emergency\":" << f.emergencyScore << ","
               << "\"fuel\":" << f.fuelTimeLeft << ","
               << "\"delay\":" << f.delayMinutes << ","
               << "\"passengers\":" << f.passengers << ","
               << "\"priority\":" << f.calculatePriority() << "}";
            first = false;
        }

        ss << "], \"nodes\": [";
        for (int i = 0; i < airport.numNodes; ++i) {
            if (i > 0) ss << ",";
            ss << "{\"id\":" << i << ",\"name\":\"" << airport.names[i] << "\"}";
        }

        ss << "], \"edges\": [";
        bool firstEdge = true;
        for (int u = 0; u < airport.numNodes; ++u) {
            for (const auto& e : airport.adj[u]) {
                if (u < e.to) { // Avoid duplicate undirected edges
                    if (!firstEdge) ss << ",";
                    ss << "{\"u\":" << u << ",\"v\":" << e.to << ",\"w\":" << e.weight << ",\"isOpen\":" << (e.isOpen ? "true" : "false") << "}";
                    firstEdge = false;
                }
            }
        }
        ss << "] }";

        res.set_content(ss.str(), "application/json");
    });

    // API: Insert / Update Flight (CO-2 Priority Queue & CO-3 Hash Map)
    svr.Post("/api/flight", [](const httplib::Request& req, httplib::Response& res) {
        lock_guard<mutex> lock(stateMutex);

        string id = extractJsonField(req.body, "id");
        int emergency = stoi(extractJsonField(req.body, "emergency"));
        int fuel = stoi(extractJsonField(req.body, "fuel"));
        int delay = stoi(extractJsonField(req.body, "delay"));
        int pax = stoi(extractJsonField(req.body, "passengers"));

        Flight f{id, emergency, fuel, delay, pax};
        flightRegistry[id] = f; // CO-3 O(1) hash map registration

        // Rebuild queue
        priority_queue<Flight, vector<Flight>, FlightComparator> freshQ;
        for (auto& pair : flightRegistry) {
            freshQ.push(pair.second);
        }
        runwayQueue = freshQ;

        res.set_content("{\"status\":\"success\"}", "application/json");
    });

    // API: Land / Dequeue Top Flight and calculate Dijkstra Route (CO-2 + CO-5)
    svr.Post("/api/land", [](const httplib::Request& req, httplib::Response& res) {
        lock_guard<mutex> lock(stateMutex);

        if (runwayQueue.empty()) {
            res.set_content("{\"status\":\"empty\"}", "application/json");
            return;
        }

        Flight landed = runwayQueue.top();
        runwayQueue.pop();
        flightRegistry.erase(landed.flightID);

        int targetGate = stoi(extractJsonField(req.body, "gate")); // 3 = Gate 10, 4 = Gate 12
        auto [cost, path] = airport.dijkstra(0, targetGate);      // From Runway-A (0)

        stringstream ss;
        ss << "{\"status\":\"ok\","
           << "\"flightID\":\"" << landed.flightID << "\","
           << "\"cost\":" << cost << ","
           << "\"path\":[";
        for (size_t i = 0; i < path.size(); ++i) {
            if (i > 0) ss << ",";
            ss << path[i];
        }
        ss << "]}";

        res.set_content(ss.str(), "application/json");
    });

    // API: Toggle Taxiway Open/Closed (CO-5 dynamic condition change)
    svr.Post("/api/toggle-edge", [](const httplib::Request& req, httplib::Response& res) {
        lock_guard<mutex> lock(stateMutex);

        int u = stoi(extractJsonField(req.body, "u"));
        int v = stoi(extractJsonField(req.body, "v"));
        string statusStr = extractJsonField(req.body, "status");
        bool status = (statusStr == "true" || statusStr == "1");

        airport.setStatus(u, v, status);
        res.set_content("{\"status\":\"updated\"}", "application/json");
    });

    cout << "========================================================\n";
    cout << "  AIRPORT ATC RUNWAY OPTIMIZER C++ SERVER ONLINE        \n";
    cout << "  Listening on: http://localhost:8080                   \n";
    cout << "========================================================\n";

    svr.listen("0.0.0.0", 8080);
    return 0;
}