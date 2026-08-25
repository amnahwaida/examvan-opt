#include "middleware/scoring.hpp"
#include <algorithm>
#include <cctype>

namespace examvan::scoring {

std::vector<Question> parse_questions(const std::string& json){
  std::vector<Question> out;
  size_t p=0;
  while(true){
    auto n=json.find("\"number\"",p); if(n==std::string::npos) break;
    auto colon=json.find(':',n); auto comma=json.find(',',colon);
    Question q; try{q.number=std::stoi(json.substr(colon+1,comma-colon-1));}catch(...){}
    auto t=json.find("\"type\"",comma); if(t!=std::string::npos){ auto c=json.find(':',t); auto q1=json.find('"',c); auto q2=json.find('"',q1+1); if(q1!=std::string::npos) q.type=json.substr(q1+1,q2-q1-1); }
    out.push_back(q);
    p=comma;
  }
  return out;
}

double score_submission(const std::vector<Question>& qs, const std::map<std::string,std::string>& answers){
  double total=0, got=0;
  for(auto& q: qs){
    total+=q.weight;
    auto it=answers.find(std::to_string(q.number));
    if(it==answers.end()) continue;
    std::string ans=it->second;
    std::transform(ans.begin(), ans.end(), ans.begin(), ::tolower);
    std::string key=q.key; std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    if(q.type=="single_choice" || q.type=="short_answer"){
      if(ans==key) got+=q.weight;
    } else if(q.type=="true_false"){
      if(ans==key) got+=q.weight;
    }
  }
  if(total==0) return 0;
  return (got/total)*100.0;
}

} // namespace examvan::scoring
