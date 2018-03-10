#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <mysql++/mysql++.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <fstream>
#include <unordered_map>

#include "json11/json11.hpp"

using std::string;

std::unique_ptr<mysqlpp::Connection> moodle_db;
std::unique_ptr<mysqlpp::Connection> ejudge_db;

struct Request {
  Request(const string &request_uri);

  struct TimeLimit {
    time_t start_time = 0;
    // MySQL time_t is 32bit.
    time_t end_time = std::numeric_limits<int32_t>::max();
  };

  std::vector<int> group_ids;
  std::vector<int> contest_ids;
  bool partial_scores = false;
  bool use_best = false;
  TimeLimit time_limit;
  std::unordered_map<int, TimeLimit> per_contest_time_limit;

  enum {
    AUTO,
    SOLVED,
    SCORE,
    NAME
  } sort_by = AUTO;
  enum {
    HTML,
    JSON
  } output = HTML;
  bool no_python = false;
};

bool g_json_output = false;

string SkipSpaces(const string &s) {
  string ret;
  ret.reserve(s.size());
  for (char i : s) {
    if (!isspace(i)) ret.push_back(i);
  }
  return ret;
}

bool StartsWith(const string &s, const string &what) {
  if (s.size() < what.size()) return false;
  for (int i = 0; i < what.size(); ++i) {
    if (s[i] != what[i]) return false;
  }
  return true;
}

bool GetContestId(const string &name, const string &prefix, int *contest_id) {
  if (StartsWith(name, prefix)) {
    *contest_id = std::stoi(name.substr(prefix.size()));
    return true;
  }
  return false;
}

Request::Request(const string &query_string) {
  std::stringstream request_stream(query_string);
  string element;
  while (std::getline(request_stream, element, '&')) {
    size_t eq_idx = element.find_first_of('=');
    if (eq_idx == 0 || eq_idx == string::npos) continue;
    string name = element.substr(0, eq_idx);
    string value = element.substr(eq_idx + 1);
    int contest_id = -1;

    if (name == "group_id") {
      group_ids.push_back(std::stoi(value));
    } else if (name == "contest_id") {
      contest_ids.push_back(std::stoi(value));
    } else if (name == "partial_scores") {
      partial_scores = value == "on";
    } else if (name == "use_best") {
      use_best = value == "on";
    } else if (name == "start_time") {
      time_limit.start_time = std::stoi(value);
    } else if (name == "end_time") {
      time_limit.end_time = std::stoi(value);
    } else if (name == "sort_by") {
      if (value == "solved")
        sort_by = SOLVED;
      else if (value == "score")
        sort_by = SCORE;
      else if (value == "name")
        sort_by = NAME;
      else
        throw std::range_error("Bad sort criterion");
    } else if (name == "output" && value == "json") {
      output = JSON;
      g_json_output = true;
    } else if (name == "no_python") {
      no_python = value == "on";
    } else if (GetContestId(name, "start_time_", &contest_id)) {
      per_contest_time_limit[contest_id].start_time = std::stoi(value);
    } else if (GetContestId(name, "end_time_", &contest_id)) {
      per_contest_time_limit[contest_id].end_time = std::stoi(value);
    }
  }

  if (sort_by == AUTO) {
    sort_by = partial_scores ? SCORE : SOLVED;
  }
}

namespace ej {
struct ProblemId {
  int contest_id = -1;
  int prob_id = -1;

  ProblemId(int contest_id = -1, int prob_id = -1)
      : contest_id(contest_id), prob_id(prob_id) {}

  bool operator<(const ProblemId &other) const {
    return std::tie(contest_id, prob_id) <
           std::tie(other.contest_id, other.prob_id);
  }
};
}  // namespace ej

struct Problem {
  int moodle_id = -1;
  string name;
  ej::ProblemId ejudge_id;
  string letter;

  int global_index = -1;
};

struct Contest {
  string name;
  std::vector<Problem> problems;
};

// Idx is zero-based.
string MakeProblemLetter(unsigned idx) {
  string ret;
  while (idx >= 26) {
    ret.push_back('A' + idx % 26);
    idx = idx / 26 - 1;
  }
  ret.push_back('A' + idx % 26);
  std::reverse(ret.begin(), ret.end());
  return ret;
}

Contest LoadContest(int id) {
  Contest contest;

  int instance_id = -1;

  {
    auto contest_info =
        moodle_db->query(
                       "SELECT mdl_statements.id, mdl_statements.name "
                       "FROM mdl_course_modules JOIN mdl_statements ON "
                       "mdl_course_modules.instance = mdl_statements.id "
                       "WHERE mdl_course_modules.id = " +
                       std::to_string(id)).store();
    if (!contest_info || contest_info.num_rows() != 1) {
      throw std::logic_error("Contest " + std::to_string(id) + " not found");
    }
    instance_id = contest_info[0][0].conv(-1);
    contest.name = string(contest_info[0][1]);
  }

  {
    moodle_db->query(
                   "SELECT mdl_problems.id, mdl_problems.name, "
                   "mdl_ejudge_problem.ejudge_contest_id, "
                   "mdl_ejudge_problem.problem_id "
                   "FROM mdl_problems "
                   "JOIN mdl_statements_problems_correlation ON "
                   "mdl_statements_problems_correlation.problem_id = "
                   "mdl_problems.id "
                   "JOIN mdl_ejudge_problem ON mdl_ejudge_problem.id = "
                   "mdl_problems.pr_id "
                   "WHERE mdl_statements_problems_correlation.statement_id = " +
                   std::to_string(instance_id) +
                   " "
                   "ORDER BY rank ASC")
        .for_each([&contest](const mysqlpp::Row &row) {
           Problem p;
           p.moodle_id = row[0];
           p.name = string(row[1]);
           p.ejudge_id.contest_id = row[2];
           p.ejudge_id.prob_id = row[3];
           p.letter = MakeProblemLetter(contest.problems.size());
           contest.problems.emplace_back(std::move(p));
         });

    if (contest.problems.size() == 0) {
      throw std::logic_error("No problems in the contest");
    }
  }

  return contest;
}

namespace ej {
struct Run {
 public:
  Run(int id, int status, int score)
      : id_(id), status_(status), score_(score) {}

  int id() const { return id_; }
  int score() const { return score_; }

  bool IsOk() const { return status_ == 0; }

  bool IsWrong() const {
    static std::set<int> wrong_statuses{2, 3, 4, 5, 6, 7};
    return wrong_statuses.find(status_) != wrong_statuses.end();
  }

  bool IsRunning() const { return status_ >= 95; }

 private:
  int id_ = -1;
  int status_ = -1;
  int score_ = 0;
};

const std::set<int> &GetContestPythonLangIds(int contest_id) {
  static std::map<int, std::set<int>> cache;
  auto it = cache.find(contest_id);
  if (it != cache.end()) {
    return it->second;
  } else {
    it = cache.emplace(contest_id, std::set<int>()).first;
  }

  std::stringstream conf_path;
  conf_path << "/home/judges/" << std::setw(6) << std::setfill('0')
            << contest_id << "/conf/serve.cfg";
  std::ifstream conf(conf_path.str());
  string current_section = "[]";
  string current_line;

  int current_lang_id = -1;
  string current_lang_name = "";

  auto end_section = [&]() {
    if (current_section == "[language]" && current_lang_id != -1) {
      if (StartsWith(current_lang_name, "\"python")) {
        it->second.insert(current_lang_id);
      }
    }
  };

  while (std::getline(conf, current_line)) {
    if (current_line.empty()) continue;
    if (current_line[0] == '#') continue;
    if (current_line[0] == '[') {
      end_section();
      current_section = current_line;
      continue;
    }
    auto eq_pos = current_line.find('=');
    if (eq_pos == string::npos || eq_pos == 0) continue;
    string key = SkipSpaces(current_line.substr(0, eq_pos - 1));
    string value = SkipSpaces(current_line.substr(eq_pos + 1));
    if (key == "id") {
      current_lang_id = std::stoi(value);
    } else if (key == "short_name") {
      current_lang_name = value;
    }
  }
  end_section();
  return it->second;
}

}  // namespace ej

struct ProblemResult {
  int tries = 0;
  int score = 0;
  bool ok = false;
  bool has_running = false;
  bool already_mentioned = false;
  bool no_submits = true;
};

ProblemResult CalculateLastProblemResult(
    const std::vector<ej::Run> &sorted_runs) {
  ProblemResult result;

  for (const auto &run : sorted_runs) {
    if (run.IsWrong()) {
      ++result.tries;
      result.score = run.score();
      result.ok = false;
    } else if (run.IsOk()) {
      result.score = run.score();
      result.ok = true;
    } else if (run.IsRunning()) {
      result.has_running = true;
    }
    result.no_submits = false;
  }

  return result;
}

ProblemResult CalculateBestProblemResult(
    const std::vector<ej::Run> &sorted_runs) {
  ProblemResult result;

  for (const auto &run : sorted_runs) {
    if (run.IsWrong()) {
      if (!result.ok) ++result.tries;
      result.score = std::max(run.score(), result.score);
    } else if (run.IsOk()) {
      result.score = std::max(run.score(), result.score);
      result.ok = true;
    } else if (run.IsRunning()) {
      result.has_running = true;
    }
    result.no_submits = false;
  }

  return result;
}

ProblemResult CalculateProblemResult(std::vector<ej::Run> runs, bool use_best) {
  std::sort(runs.begin(), runs.end(),
            [](const ej::Run &lhs,
               const ej::Run &rhs) { return lhs.id() < rhs.id(); });
  return (use_best ? CalculateBestProblemResult
                   : CalculateLastProblemResult)(runs);
}

struct User {
  string name;
  std::vector<ProblemResult> results;

  int solved = 0, score = 0;
  int seq_number = 0;
};

void AddOneUser(const mysqlpp::Row &row, std::map<int, User> *users) {
  User u;
  u.name = string(row[1]);
  users->emplace(int(row[0]), u);
}

void AddUsersFromGroup(int group_id, std::map<int, User> *users) {
  moodle_db->query(
                 "SELECT "
                 "mdl_user.ej_id, "
                 "CONCAT(mdl_user.lastname, ' ', mdl_user.firstname) "
                 "FROM mdl_user "
                 "JOIN mdl_ejudge_group_users ON "
                 "mdl_ejudge_group_users.user_id=mdl_user.id "
                 "WHERE mdl_ejudge_group_users.group_id = " +
                 std::to_string(group_id))
      .for_each([users](const mysqlpp::Row &row) { AddOneUser(row, users); });
}

typedef std::vector<Contest> Contests;
typedef std::vector<User> Users;

struct Monitor {
  std::vector<Contest> contests;
  std::vector<User> users;
};

Monitor GetMonitor(const Request &r) {
  Monitor monitor;
  std::vector<Contest> &contests = monitor.contests;
  std::map<int, User> users_by_ejudge_id;

  std::map<ej::ProblemId, std::vector<int>> ej_problem_to_global_index;

  for (auto contest_id : r.contest_ids) {
    contests.emplace_back(LoadContest(contest_id));
  }

  string problems_filter = "(FALSE";

  int global_index_seq = 0;
  int contest_id_seq = 1;
  for (auto &contest : contests) {
    for (auto &problem : contest.problems) {
      problem.letter =
          std::to_string(contest_id_seq) + /*"-" +*/ problem.letter;
      problem.global_index = global_index_seq++;
      ej_problem_to_global_index[problem.ejudge_id]
          .push_back(problem.global_index);

      problems_filter.append(" OR (contest_id = " +
                             std::to_string(problem.ejudge_id.contest_id) +
                             ") AND (prob_id = " +
                             std::to_string(problem.ejudge_id.prob_id) + ")");
    }
    ++contest_id_seq;
  }

  problems_filter.push_back(')');

  for (auto group_id : r.group_ids) {
    AddUsersFromGroup(group_id, &users_by_ejudge_id);
  }

  string users_filter = "(user_id IN (-1";

  for (auto &user : users_by_ejudge_id) {
    user.second.results.resize(global_index_seq);

    users_filter.append(", " + std::to_string(user.first));
  }

  users_filter.append("))");

  std::map<std::pair<int, ej::ProblemId>, std::vector<ej::Run>>
      runs_by_user_and_problem;

  string time_filter = " (create_time > FROM_UNIXTIME(" +
                       std::to_string(r.time_limit.start_time) +
                       ") AND create_time < FROM_UNIXTIME(" +
                       std::to_string(r.time_limit.end_time) + ")) ";

  string ej_query =
      "SELECT "
      "run_id, contest_id, prob_id, user_id, status, score, lang_id, "
      "create_time "
      "FROM runs "
      "WHERE " +
      problems_filter + " AND " + users_filter + " AND " + time_filter;
  std::cerr << "ej query: " << ej_query << std::endl;

  ejudge_db->query(ej_query)
      .for_each([&runs_by_user_and_problem, &r](const mysqlpp::Row &row) {
         int run_id = row[0];
         int contest_id = row[1];
         int prob_id = row[2];
         int user_id = row[3];
         int status = row[4];
         int score = row[5];
         int lang_id = row[6];
         time_t create_time = row[7];

         bool python_allowed =
             !r.no_python ||
             ej::GetContestPythonLangIds(contest_id).count(lang_id) == 0;

         auto time_limit = r.per_contest_time_limit.find(contest_id);
         bool time_allowed = time_limit == r.per_contest_time_limit.end() ||
                             (time_limit->second.start_time <= create_time &&
                              create_time <= time_limit->second.end_time);

         if (python_allowed && time_allowed) {
           runs_by_user_and_problem[{user_id, {contest_id, prob_id}}]
               .emplace_back(run_id, status, score);
         }
       });

  for (const auto &runs : runs_by_user_and_problem) {
    auto results = CalculateProblemResult(runs.second, r.use_best);
    for (auto &idx : ej_problem_to_global_index.at(runs.first.second)) {
      users_by_ejudge_id.at(runs.first.first).results.at(idx) = results;
      results.already_mentioned = true;
    }
  }

  std::vector<User> &users = monitor.users;
  for (auto &user : users_by_ejudge_id) {
    bool no_submits = true;
    for (const auto &result : user.second.results) {
      if (result.already_mentioned) continue;
      if (result.ok) ++user.second.solved;
      user.second.score += result.score;
      no_submits &= result.no_submits;
    }

    if (!no_submits) {
      users.emplace_back(std::move(user.second));
    }
  }

  if (r.sort_by == r.SOLVED) {
    std::sort(users.begin(), users.end(), [](const User &lhs, const User &rhs) {
      return std::make_pair(-lhs.solved, lhs.name) <
             std::make_pair(-rhs.solved, rhs.name);
    });
  } else if (r.sort_by == r.SCORE) {
    std::sort(users.begin(), users.end(), [](const User &lhs, const User &rhs) {
      return std::make_pair(-lhs.score, lhs.name) <
             std::make_pair(-rhs.score, rhs.name);
    });
  } else if (r.sort_by == r.NAME) {
    std::sort(users.begin(), users.end(), [](const User &lhs, const User &rhs) {
      return lhs.name < rhs.name;
    });
  }

  if (!users.empty()) users[0].seq_number = 1;
  int seq = 1;
  for (auto prev = users.begin(), cur = !users.empty() ? std::next(prev) : prev;
       cur != users.end(); ++cur, ++prev) {
    ++seq;
    if ((r.sort_by == r.SOLVED && cur->solved == prev->solved) ||
        (r.sort_by == r.SCORE && cur->score == prev->score)) {
      cur->seq_number = prev->seq_number;
    } else {
      cur->seq_number = seq;
    }
  }

  return monitor;
}

void RenderHtml(const Request &r, const Monitor &monitor) {
  const auto &contests = monitor.contests;
  const auto &users = monitor.users;
  std::vector<std::vector<string>> table;

  std::vector<string> header{"N", "Name", "Sum"};
  for (const auto &contest : contests) {
    for (const auto &problem : contest.problems) {
      header.push_back(
          "<a "
          "href=\"http://informatics.mccme.ru/mod/statements/"
          "view3.php?chapterid=" +
          std::to_string(problem.moodle_id) + "\">" + problem.letter + "</a>");
    }
  }

  table.emplace_back(std::move(header));

  for (const auto &user : users) {
    std::vector<string> row;
    row.emplace_back(std::to_string(user.seq_number));
    row.emplace_back(user.name);
    row.emplace_back(
        std::to_string(r.partial_scores ? user.score : user.solved));
    for (const auto &problem : user.results) {
      string desc;
      if (r.partial_scores) {
        if (problem.score) {
          desc = std::to_string(problem.score);
        }
      } else {
        if (problem.ok) {
          desc = "+";
        } else if (!problem.ok && problem.tries > 0) {
          desc = "-";
        }
        if (problem.tries > 0) {
          desc += std::to_string(problem.tries);
        }
      }
      if (problem.already_mentioned) {
        desc = "(" + desc + ")";
      }
      row.emplace_back(desc);
    }
    table.emplace_back(std::move(row));
  }
  std::cout << "Content-type: text/html\r\n\r\n";
  std::cout << R"(<style>
    table.BlueTable {
    border: 1px solid #CCCCCC;
    border-right: 0px;
    border-bottom: 0px;
    font-family: Verdana;
    font-size: 10pt;
  }
  </style>)";
  std::cout << "<meta http-equiv=\"Content-type\" content=\"text/html; "
               "charset=utf-8\" />";
  std::cout
      << "<table class=\"BlueTable\" cellspacing=0 cellpadding=2 border=1>";
  for (const auto &row : table) {
    std::cout << "<tr>";
    for (const auto &i : row) {
      string color;
      if (i == "100" || (!i.empty() && i[0] == '+')) {
        color = " bgcolor=\"#e1f2e1\"";
      }
      std::cout << "<td" << color << ">" << i << "</td>";
    }
    std::cout << "</tr>";
  }
  std::cout << "</table>" << std::endl;

  std::cout << "<table border=1><tr><td>Letter</td><td>Name</td></tr>";
  for (const auto &contest : contests) {
    std::cout << "<tr><td>"
              << "Contest"
              << "</td><td>" << contest.name << "</td></tr>";
    for (const auto &problem : contest.problems) {
      std::cout << "<tr><td>" << problem.letter << "</td><td>" << problem.name
                << "</td></tr>";
    }
  }
  std::cout << "</table>";
}

void RenderJson(const Request &r, const Monitor &m) {
  using json11::Json;

  Json::object root;

  root["request"] = Json::object(
      {{"partial_scores", r.partial_scores}, {"use_best", r.use_best}, });

  Json::array contests;
  for (const auto &contest : m.contests) {
    Json::array problems;
    for (const auto &problem : contest.problems) {
      problems.push_back(Json::object{{"global_index", problem.global_index},
                                      {"letter", problem.letter},
                                      {"name", problem.name}, });
    }
    contests.push_back(
        Json::object{{"name", contest.name}, {"problems", problems}, });
  }
  root["contests"] = std::move(contests);

  Json::array users;
  for (const auto &user : m.users) {
    Json::array results;
    for (const auto &result : user.results) {
      results.push_back(Json::object{{"already_mentioned",
                                      result.already_mentioned},
                                     {"has_running", result.has_running},
                                     {"ok", result.ok}, {"score", result.score},
                                     {"tries", result.tries}, });
    }
    users.push_back(Json::object{{"name", user.name},
                                 {"results", results},
                                 {"score", user.score},
                                 {"seq_number", user.seq_number},
                                 {"solved", user.solved}, });
  }
  root["users"] = users;

  std::cout << "Content-type: application/json\r\n\r\n" << Json(root).dump()
            << std::endl;
}

int main(int ac, char **av) {
  try {
    string query_string;
    if (auto *env = getenv("QUERY_STRING")) {
      query_string = env;
    } else if (ac == 2) {
      query_string = av[1];
    } else {
      // No parameters.
    }

    moodle_db.reset(new mysqlpp::Connection);
    moodle_db->set_option(new mysqlpp::SetCharsetNameOption("utf8"));
    moodle_db->connect("moodle", "localhost", "root", "");

    ejudge_db.reset(new mysqlpp::Connection);
    ejudge_db->set_option(new mysqlpp::SetCharsetNameOption("utf8"));
    ejudge_db->connect("ejudge", "localhost", "root", "");

    const Request r(query_string);

    Contests contests;
    Users users;

    auto monitor = GetMonitor(r);

    if (r.output == r.JSON) {
      RenderJson(r, monitor);
    } else if (r.output == r.HTML) {
      RenderHtml(r, monitor);
    }
  }
  catch (std::exception &e) {
    // TODO: check g_json_output
    std::cout << "Content-type: text/plain; charset=utf8\r\n\r\nError:\n"
              << e.what() << std::endl;
  }
}
