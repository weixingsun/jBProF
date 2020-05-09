#include "gp/gp.h"
#include "gp/rprop.h"
#include "gp/cg.h"
#include "gp/gp_utils.h"

#include <cmath>
#include <iomanip>
#include <iostream>

using namespace std;
using Matrix = Eigen::MatrixXd;
using Vector = Eigen::VectorXd;
using GP = libgp::GaussianProcess;

double fin(double X){
    return -sin(3*X) - X*X + 0.7*X;
}
Vector finM(Matrix X){
    int r = X.rows();
    Vector V(r);
    int c = X.cols();
    double C = 0;
    for (int i = 0; i < r; i++){
        for (int j = 0; j < c; j++){
            C += fin(X(i,j)) ;
        }
        V[i]=C;
        C=0;
    }
    return V;
}
Vector square(Matrix X){
    int r = X.rows();
    Vector V(r);
    int c = X.cols();
    auto C = X(0,0);
    C=0;
    for (int i = 0; i < r; ++i){
        for (int j = 0; j < c; ++j){
            C += X(i,j)*X(i,j);
        }
        V[i]=C;
        C=0;
    }
    return V;
}

Matrix init_data(int N, int input_dim){
    Matrix X(N, input_dim);
    X.setRandom();
    X = X*2;
    cout<<" ---------------------------------init X ("<<X.rows()<<","<<X.cols()<<")"<<endl;
    int R = X.rows();
    int C = X.cols();
    for(int i=0;i<R;i++){
       cout<<i<<"[ ";
       for(int j=0;j<C;j++){
          cout<<X(i,j)<<", ";
       }
       cout<<" ]"<<endl;
    }
    return X;
}
GP* init( Matrix X, Vector Y) {
    //Matrix mat = Matrix::Random(2, 3);
    GP *gp = new GP(X.rows(), "CovSum ( CovSEiso, CovNoise)");
    Vector params(gp->covf().get_param_dim());
    params << 0, 0, -2;
    gp->covf().set_loghyper(params);
    
    //Vector y = gp->covf().draw_random_sample(X);
    for(size_t i = 0; i < X.rows(); ++i) {
        double x[X.cols()];
        for(int j = 0; j < X.cols(); ++j) {
            x[j] = X(i,j);
            //cout<<"x,y = \t"<<x[j]<<"\t"<<Y(i)<<endl;
        }
        gp->add_pattern(x, Y(i));
    }
    return gp;
}

double validate(GP *gp, double x1, double x2){
  double x[2] = {x1,x2};
  double y = gp->f(x);
  double var = gp->var(x);
  cout<<"f("<<x1<<")\t="<<y<<"\t""var="<<var<<endl;
  return var;
}
void validate_all(GP *gp){
  cout<<"-------------------------------"<<endl;
  validate(gp,  -1,0);
  validate(gp,-0.5,0);
  validate(gp,   0,0);
  validate(gp, 0.5,0);
  validate(gp,   1,0);
  validate(gp, 1.5,0);
  validate(gp,   2,0);
  cout<<"-------------------------------"<<endl;
}

void fit1(GP *gp){
  libgp::RProp rprop;
  rprop.init();
  int ver = 0;
  int prt = 0;
  rprop.maximize(gp, 50, ver, prt);
}
///////////////////////////////////////////
//finM
Vector predict(GP *gp, Matrix X, bool var){
    Vector y(X.rows());
    for(int i = 0; i < X.rows(); i++) {
        double x[X.cols()];
        for(int j = 0; j < X.cols(); j++) {
            x[j] = X(i,j);
        }
        if(var) y[i] = gp->var(x);
        else    y[i] = gp->f(x);
    }
    return y;
}
double best_vec(Vector v, bool top_best){
    double max = v[0];
    for(int j = 1; j < v.size(); j++) {
        if(top_best && max>v[j])  max = v[j];
        else if(!top_best && max<v[j]) max = v[j];
    }
    return max;
}
double sum_vec(Vector v){
    double sum = v[0];
    for(int j = 1; j < v.size(); j++) {
        sum += v[j];
    }
    return sum;
}
double expectedImprovement(GP *gp, Vector x, double target) {
    double g = (gp->f(x.data()) - target) / sqrt(gp->var(x.data()));
    double ei = sqrt(gp->var(x.data())) * (g * libgp::Utils::cdf_norm(g)   +   1.0/(2*M_PI) * exp(-0.5*g*g)   );
    return ei;
}
Vector expectedImprovement(GP *gp, Matrix x, Vector target) {
    Vector y(x.rows());
    for(int i = 0; i < x.rows(); i++) {
        double d = expectedImprovement(gp, x.row(i), target[i] );
        y[i]=d;
    }
    return y;
}
//double probabilityImprovement(libgp::GaussianProcess *gp, Eigen::VectorXd x, double target) {
//    return libgp::Utils::cdf_norm( (gp->f(x.data()) - target - 0.01) / gp->var(x.data()));
//}
void expected_improvement(GP *gp, Matrix X, Matrix Xsample, Vector Ysample, double buf=0.01){
    bool top_best = true;
    Vector mu = predict( gp, X, false);
    Vector sigma = predict( gp, X, true);
    Vector mu_sample = predict(gp,Xsample,false);
    double mu_sample_best = best_vec(mu_sample,top_best);
    //double scaling_factor = -1 ** top_best;
    //Vector I = mu - mu_sample_best ;  //-buf
    //Vector Z = I / sigma;
    //////////////Vector ei = expectedImprovement(gp, Xsample, Ysample);
}

//batch: 7 candidates in norm dist |   | ||| |   |
void next_sample(GP *gp, Matrix x, Vector y, Vector lb, Vector ub, int restart=10){
    //x.resize(m,n);
    Vector mu = predict( gp, x, false);
    Vector sigma = predict( gp, x, true);

    double maxY = 0;
    double maxX = 0;
    for (int i=0; i<5; i++){
        double x[2] = {lb[0],0}; //lb[1]};
        double y   = gp->f(x);
        double var = gp->var(x);
        if( y-var > maxY ) {
            maxY = y;
            maxX = x[0];
        }
        cout<<"iterator="<<i<<"  x1="<<x[0]<<" -> "<<y<<"  ("<<var<<")"<<" X="x[0]<<"  Y="<<maxY<<endl;
    }
}
int main(){
    int input_dim=1;
    int N=10;
    Matrix X = init_data(N,input_dim);
    Vector Y = finM(X);
    cout<<"Y=\n"<<Y<<endl;
    GP *gp = init(X,Y);
    if(validate(gp,0.5,0)>0.9){
        cout<<"---------------------------------------- re-init"<<endl;
        init(X,Y);
    }

    cout<<"before training"<<endl;
    validate_all(gp);
    fit1(gp);
    cout<<"after training"<<endl;
    validate_all(gp);

    Vector lb = Vector::Constant(X.rows(),-1);
    Vector ub = Vector::Constant(X.rows(),2);
    //cout<<"lb=\n"<<lb<<"  ub=\n"<<ub<<endl;
    next_sample(gp, X, Y, lb,ub);
}
