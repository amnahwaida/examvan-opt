#include "middleware/scoring.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace examvan::scoring {

static std::string extract_str(const std::string& obj, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=obj.size();
  bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && obj.compare(i, needle.size(), needle)==0){
      size_t colon=i+needle.size();
      while(colon<n && (obj[colon]==' '||obj[colon]=='\t'||obj[colon]=='\n'||obj[colon]=='\r')) colon++;
      if(colon<n && obj[colon]==':'){
        size_t v=colon+1;
        while(v<n && (obj[v]==' '||obj[v]=='\t'||obj[v]=='\n'||obj[v]=='\r')) v++;
        if(v<n && obj[v]=='"'){
          size_t q1=v;
          size_t q2=q1+1; while(q2<n){ if(obj[q2]=='\\'){ q2+=2; continue; } if(obj[q2]=='"') break; q2++; }
          if(q2<n) return obj.substr(q1+1,q2-q1-1);
        }
      }
    }
    char c=obj[i];
    if(esc){ esc=false; }
    else if(c=='\\' && in_str){ esc=true; }
    else if(c=='"'){ in_str=!in_str; }
    i++;
  }
  return "";
}
static double extract_double(const std::string& obj, const std::string& key, double def){
  std::string needle="\""+key+"\"";
  size_t n=obj.size();
  bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && obj.compare(i, needle.size(), needle)==0){
      size_t colon=i+needle.size();
      while(colon<n && (obj[colon]==' '||obj[colon]=='\t'||obj[colon]=='\n'||obj[colon]=='\r')) colon++;
      if(colon<n && obj[colon]==':'){
        size_t s=colon+1;
        while(s<n && (obj[s]==' '||obj[s]=='\t'||obj[s]=='\n'||obj[s]=='\r')) s++;
        if(s<n){
          if(obj[s]=='"'){ auto v=extract_str(obj,key); try{return std::stod(v);}catch(...){return def;}}
          else { size_t e=s; while(e<n && obj[e]!=',' && obj[e]!='}' ) e++; try{return std::stod(obj.substr(s,e-s));}catch(...){return def;}}
        }
      }
    }
    char c=obj[i];
    if(esc){ esc=false; }
    else if(c=='\\' && in_str){ esc=true; }
    else if(c=='"'){ in_str=!in_str; }
    i++;
  }
  return def;
}
static int extract_int(const std::string& obj, const std::string& key, int def){
  return (int)extract_double(obj,key,def);
}
// Nilai JSON mentah sebuah kolom (string ber-quote, array, objek, atau skalar).
static std::string extract_raw_value(const std::string& obj, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=obj.find(needle);
  if(p==std::string::npos) return "";
  size_t c=obj.find(':', p+needle.size());
  if(c==std::string::npos) return "";
  size_t i=c+1;
  while(i<obj.size() && (obj[i]==' '||obj[i]=='\t'||obj[i]=='\n'||obj[i]=='\r')) i++;
  if(i>=obj.size()) return "";
  char ch=obj[i];
  if(ch=='"'){
    size_t e=i+1; while(e<obj.size()){ if(obj[e]=='\\'){e+=2;continue;} if(obj[e]=='"') break; e++; }
    if(e<obj.size()) return obj.substr(i, e-i+1);
    return "";
  }
  if(ch=='[' || ch=='{'){
    int depth=0; bool in=false, esc=false;
    for(size_t j=i;j<obj.size();++j){
      char cj=obj[j];
      if(esc){esc=false;continue;}
      if(cj=='\\'&&in){esc=true;continue;}
      if(cj=='"'){in=!in;continue;}
      if(in) continue;
      if(cj=='['||cj=='{') depth++;
      else if(cj==']'||cj=='}'){ depth--; if(depth==0) return obj.substr(i, j-i+1); }
    }
    return "";
  }
  size_t e=i; while(e<obj.size() && obj[e]!=',' && obj[e]!='}' && obj[e]!=']') e++;
  return obj.substr(i, e-i);
}

static bool json_bool_field(const std::string& obj, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=obj.find(needle);
  if(p==std::string::npos) return false;
  size_t c=obj.find(':', p);
  if(c==std::string::npos) return false;
  return obj.compare(c+1, 4, "true")==0;
}

std::vector<Question> parse_questions(const std::string& json){
  std::vector<Question> out;
  size_t pos=0;
  while(true){
    auto a=json.find('{',pos); if(a==std::string::npos) break;
    size_t b=a;
    int depth=0;
    bool in_str=false;
    bool esc=false;
    for(; b<json.size(); ++b){
      char c=json[b];
      if(esc){ esc=false; continue; }
      if(c=='\\' && in_str){ esc=true; continue; }
      if(c=='"'){ in_str=!in_str; continue; }
      if(in_str) continue;
      if(c=='{') depth++;
      else if(c=='}'){ depth--; if(depth==0) break; }
    }
    if(b>=json.size() || depth!=0) break;
    std::string obj=json.substr(a,b-a+1);
    if(obj.find("\"number\"")!=std::string::npos){
      Question q;
      q.number=extract_int(obj,"number",0);
      q.type=extract_str(obj,"type");
      if(q.type.empty()) q.type="single_choice";
      q.weight=extract_double(obj,"weight",1);
      q.key=extract_str(obj,"key");
      if(q.key.empty()) q.key=extract_str(obj,"answer");
      // partial_scoring (Go) / partial (legacy) / tipe "partial" (legacy).
      q.partial=json_bool_field(obj,"partial_scoring") || json_bool_field(obj,"partial") || q.type=="partial";
      q.key_raw=extract_raw_value(obj,"key");
      if(q.key_raw.empty()) q.key_raw=extract_raw_value(obj,"answer");
      if(q.key_raw.empty() && !q.key.empty()) q.key_raw="\""+q.key+"\"";
      out.push_back(q);
    }
    pos=b+1;
  }
  return out;
}

std::optional<double> score_submission_json(const std::string& questions_json, const std::map<std::string,std::string>& answers){
  if(questions_json.empty()) return std::nullopt;
  auto qs=parse_questions(questions_json);
  if(qs.empty()) return std::nullopt;
  return score_submission(qs, answers);
}

double score_submission(const std::vector<Question>& qs, const std::map<std::string,std::string>& answers){
  double total=0, got=0;
  for(auto& q: qs){
    total+=q.weight;
    auto it=answers.find(std::to_string(q.number));
    if(it==answers.end()) continue;
    // Satu sumber kebenaran: evaluate_question_detail (paritas Go) — dulu
    // path ini memakai rumus berbeda utk partial multi-select (hit/total
    // tanpa penalti jawaban salah) sehingga skor tersimpan tidak konsisten
    // dgn detail per-soal (max(0, benar-salah)/total).
    EvalDetail d=evaluate_question_detail(q, it->second);
    got+=d.earned;
  }
  if(total==0) return 0;
  return (got/total)*100.0;
}

// ===== Evaluasi detail per-soal (paritas Go evaluateSingleQuestion) =====
namespace {

// Go: strings.ToUpper(strings.Join(strings.Fields(s), " "))
std::string norm_ans(const std::string& s){
  std::string out;
  bool space=false;
  for(char c: s){
    if(std::isspace((unsigned char)c)){ space=true; }
    else { if(space && !out.empty()) out.push_back(' '); out.push_back((char)std::toupper((unsigned char)c)); space=false; }
  }
  return out;
}

// Token dari raw JSON array (["A","C"]) atau string "A,C;D" / "A".
std::vector<std::string> parse_set_tokens(const std::string& raw){
  std::vector<std::string> out;
  size_t a=raw.find('['), b=raw.rfind(']');
  std::string inner=(a!=std::string::npos && b!=std::string::npos && b>a) ? raw.substr(a+1, b-a-1) : raw;
  size_t i=0;
  while(i<inner.size()){
    while(i<inner.size() && (inner[i]==' '||inner[i]=='\t'||inner[i]==','||inner[i]==';'||inner[i]=='\n'||inner[i]=='\r')) i++;
    if(i>=inner.size()) break;
    std::string tok;
    if(inner[i]=='"'){
      i++;
      while(i<inner.size()){ if(inner[i]=='\\'){ i+=2; continue; } if(inner[i]=='"'){ i++; break; } tok+=inner[i++]; }
    } else {
      while(i<inner.size() && inner[i]!=',' && inner[i]!=';' && inner[i]!=']' && inner[i]!='\t' && inner[i]!='\n' && inner[i]!='\r') tok+=inner[i++];
    }
    if(!tok.empty()) out.push_back(norm_ans(tok));
  }
  return out;
}

struct KV { std::string k, v; };
// Pasangan key→value dari raw JSON objek ({"1":"A"}) atau string "1:A,2:B".
std::vector<KV> parse_object_pairs(const std::string& raw){
  std::vector<KV> out;
  size_t a=raw.find('{'), b=raw.rfind('}');
  std::string in=(a!=std::string::npos && b!=std::string::npos && b>a) ? raw.substr(a+1, b-a-1) : raw;
  size_t i=0;
  while(i<in.size()){
    while(i<in.size() && (in[i]==' '||in[i]=='\t'||in[i]==','||in[i]=='\n'||in[i]=='\r')) i++;
    if(i>=in.size()) break;
    std::string k;
    if(in[i]=='"'){ i++; while(i<in.size()){ if(in[i]=='\\'){i+=2;continue;} if(in[i]=='"'){i++;break;} k+=in[i++]; } }
    else { while(i<in.size() && in[i]!=':' && in[i]!=',') k+=in[i++]; }
    while(i<in.size() && in[i]!=':') i++;
    if(i<in.size()) i++;
    while(i<in.size() && (in[i]==' '||in[i]=='\t'||in[i]=='\n'||in[i]=='\r')) i++;
    std::string v;
    if(i<in.size() && in[i]=='"'){ i++; while(i<in.size()){ if(in[i]=='\\'){i+=2;continue;} if(in[i]=='"'){i++;break;} v+=in[i++]; } }
    else { while(i<in.size() && in[i]!=',' && in[i]!='}') v+=in[i++]; }
    if(!k.empty()) out.push_back({norm_ans(k), norm_ans(v)});
  }
  return out;
}

bool sets_equal(const std::vector<std::string>& a, const std::vector<std::string>& b){
  if(a.size()!=b.size()) return false;
  std::vector<std::string> x=a, y=b;
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return x==y;
}

bool sets_equal_obj(const std::vector<KV>& a, const std::vector<KV>& b){
  if(a.size()!=b.size()) return false;
  for(const auto& x: a){
    bool found=false;
    for(const auto& y: b) if(y.k==x.k && y.v==x.v){ found=true; break; }
    if(!found) return false;
  }
  return true;
}

std::string single_key_token(const Question& q){
  auto toks=parse_set_tokens(q.key_raw);
  if(toks.empty()) return norm_ans(q.key);
  return toks.front();
}

} // namespace

EvalDetail evaluate_question_detail(const Question& q, const std::string& answer_raw){
  EvalDetail d;
  std::string ans=answer_raw;
  {
    size_t a0=ans.find_first_not_of(" \t\r\n");
    if(a0==std::string::npos) ans=""; else ans=ans.substr(a0);
  }
  if(ans.empty() || ans=="[]" || ans=="{}"){
    d.earned=0; d.status="unanswered"; return d;
  }
  std::string type=q.type;
  if(type=="multiple_choice" || type=="multiple_answer" || type=="partial"){
    auto correct=parse_set_tokens(q.key_raw);
    auto student=parse_set_tokens(ans);
    if(q.partial){
      if(correct.empty()){ d.status="incorrect"; return d; }
      int corr=0, wrong=0;
      for(const auto& s: student)
        if(std::find(correct.begin(), correct.end(), s)!=correct.end()) corr++; else wrong++;
      double portion=std::max(0.0, double(corr-wrong))/double(correct.size());
      d.earned=portion*q.weight;
      if(portion>=1.0-1e-9) d.status="correct";
      else if(portion>1e-9) d.status="partial";
      else d.status="incorrect";
    } else {
      if(sets_equal(student, correct)){ d.earned=q.weight; d.status="correct"; }
      else { d.earned=0; d.status="incorrect"; }
    }
    return d;
  }
  if(type=="matching"){
    auto correct=parse_object_pairs(q.key_raw);
    auto student=parse_object_pairs(ans);
    if(q.partial){
      if(correct.empty()){ d.status="incorrect"; return d; }
      int hit=0;
      for(const auto& c: correct)
        for(const auto& s: student)
          if(s.k==c.k && s.v==c.v){ hit++; break; }
      double portion=double(hit)/double(correct.size());
      d.earned=portion*q.weight;
      if(portion>=1.0-1e-9) d.status="correct";
      else if(portion>1e-9) d.status="partial";
      else d.status="incorrect";
    } else {
      if(sets_equal_obj(correct, student)){ d.earned=q.weight; d.status="correct"; }
      else { d.earned=0; d.status="incorrect"; }
    }
    return d;
  }
  // single_choice / true_false / short_answer / lainnya: exact.
  std::string cNorm=single_key_token(q);
  if(norm_ans(ans)==cNorm){ d.earned=q.weight; d.status="correct"; }
  else { d.earned=0; d.status="incorrect"; }
  return d;
}

} // namespace examvan::scoring
