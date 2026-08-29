#include "middleware/scoring.hpp"
#include <algorithm>
#include <cctype>

namespace examvan::scoring {

static std::string extract_str(const std::string& obj, const std::string& key){
  std::string needle="\""+key+"\"";
  auto p=obj.find(needle); if(p==std::string::npos) return "";
  auto c=obj.find(':',p+needle.size()); if(c==std::string::npos) return "";
  size_t q1=obj.find('"',c); if(q1==std::string::npos) return "";
  size_t q2=q1+1; while(q2<obj.size()){ if(obj[q2]=='\\'){ q2+=2; continue; } if(obj[q2]=='"') break; q2++; }
  if(q2>=obj.size()) return "";
  return obj.substr(q1+1,q2-q1-1);
}
static double extract_double(const std::string& obj, const std::string& key, double def){
  std::string needle="\""+key+"\"";
  auto p=obj.find(needle); if(p==std::string::npos) return def;
  auto c=obj.find(':',p+needle.size()); if(c==std::string::npos) return def;
  size_t s=obj.find_first_not_of(" \t",c+1); if(s==std::string::npos) return def;
  if(obj[s]=='"'){ auto v=extract_str(obj,key); try{return std::stod(v);}catch(...){return def;}}
  size_t e=obj.find_first_of(",}",s); if(e==std::string::npos) e=obj.size();
  try{return std::stod(obj.substr(s,e-s));}catch(...){return def;}
}
static int extract_int(const std::string& obj, const std::string& key, int def){
  return (int)extract_double(obj,key,def);
}
std::vector<Question> parse_questions(const std::string& json){
  std::vector<Question> out;
  size_t pos=0;
  while(true){
    auto a=json.find('{',pos); if(a==std::string::npos) break;
    auto b=json.find('}',a); if(b==std::string::npos) break;
    std::string obj=json.substr(a,b-a+1);
    if(obj.find("\"number\"")!=std::string::npos){
      Question q;
      q.number=extract_int(obj,"number",0);
      q.type=extract_str(obj,"type");
      if(q.type.empty()) q.type="single_choice";
      q.weight=extract_double(obj,"weight",1);
      q.key=extract_str(obj,"key");
      if(q.key.empty()) q.key=extract_str(obj,"answer");
      out.push_back(q);
    }
    pos=b+1;
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
    auto trim=[](std::string s){ s.erase(0,s.find_first_not_of(" \t")); if(!s.empty()) s.erase(s.find_last_not_of(" \t")+1); return s; };
    ans=trim(ans); key=trim(key);
    if(q.type=="single_choice" || q.type=="short_answer" || q.type=="true_false"){
      if(ans==key) got+=q.weight;
    } else if(q.type=="multiple_choice" || q.type=="multiple_answer" || q.type=="partial"){
      auto split=[&](const std::string& s){ std::vector<std::string> out; std::string cur; for(char c: s){ if(c==','||c==';'){ if(!cur.empty()){ out.push_back(trim(cur)); cur.clear(); } } else cur.push_back(c);} if(!cur.empty()) out.push_back(trim(cur)); return out; };
      auto ans_parts=split(ans);
      auto key_parts=split(key);
      std::sort(ans_parts.begin(), ans_parts.end());
      std::sort(key_parts.begin(), key_parts.end());
      if(q.type=="partial"){
        size_t hit=0; for(auto& a: ans_parts) if(std::find(key_parts.begin(), key_parts.end(), a)!=key_parts.end()) hit++;
        if(!key_parts.empty()) got+=q.weight * ((double)hit / key_parts.size());
      } else {
        if(ans_parts==key_parts) got+=q.weight;
      }
    } else {
      if(ans==key) got+=q.weight;
    }
  }
  if(total==0) return 0;
  return (got/total)*100.0;
}

} // namespace examvan::scoring
