#include "websocket/socketio.hpp"
#include <sstream>

namespace examvan {

std::string json_string(const std::string& s) {
  std::string out="\"";
  for(char c: s){
    if(c=='"') out+="\\\"";
    else if(c=='\\') out+="\\\\";
    else if(c=='\n') out+="\\n";
    else if(c=='\r') out+="\\r";
    else out+=c;
  }
  out+="\"";
  return out;
}

std::string make_payload(const std::map<std::string,std::string>& kv){
  std::string j="{";
  bool first=true;
  for(auto& [k,v]:kv){ if(!first) j+=","; first=false; j+=json_string(k)+":"+json_string(v); }
  j+="}";
  return j;
}

std::string marshal_socketio(const std::string& event, const std::string& payload_json){
  return "[" + json_string(event) + "," + payload_json + "]";
}

static size_t find_closing_quote(const std::string& s, size_t start){
  for(size_t i=start+1;i<s.size();++i){
    if(s[i]=='\\'){ ++i; continue; }
    if(s[i]=='"') return i;
  }
  return std::string::npos;
}
std::optional<SocketIOMessage> parse_socketio(const std::string& data){
  if(data.size()<4 || data.front()!='[') return std::nullopt;
  size_t p1 = data.find('"',0);
  if(p1==std::string::npos) return std::nullopt;
  size_t p2 = find_closing_quote(data, p1);
  if(p2==std::string::npos) return std::nullopt;
  std::string event = data.substr(p1+1, p2-p1-1);
  size_t comma = data.find(',',p2);
  if(comma==std::string::npos) return std::nullopt;
  size_t start = data.find_first_not_of(" \t",comma+1);
  if(start==std::string::npos) return std::nullopt;
  size_t end = data.rfind(']');
  if(end==std::string::npos || end<=start) return std::nullopt;
  std::string payload = data.substr(start, end-start);
  return SocketIOMessage{event,payload};
}

}  // namespace examvan
